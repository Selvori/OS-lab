#!/usr/bin/env python3
"""Build paired QEMU guests and preserve bounded-template raw data."""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from bench_window_reclaim import lines
import mkfs
import test_lab4 as qemu


def sample(vm, command, success):
    output = lines(vm, command)
    if success not in output or "USER FAULT" in output or "FAIL " in output:
        raise AssertionError(output[-6000:])
    rows = defaultdict(list)
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and all(piece.isdecimal() for piece in parts[1:]):
            rows[parts[0]].append(list(map(int, parts[1:])))
    if command.startswith("bclose "):
        _, idle, start, _, quiet, *_ = rows["CLOSE"][0]
        assert [r[0] for r in rows["STORE"]] == list(range(2048))
        assert [r[0] for r in rows["FRAME"]] == list(range(2048))
        assert len({r[1] for r in rows["FRAME"]}) == 2048
        if not idle:
            progress = rows["PROGRESS"]
            assert 1 < len(progress) < 4096
            assert [r[0] for r in progress] == list(range(len(progress)))
            assert progress[0][1] <= start <= quiet <= progress[-1][1]
    return dict(command=command, rows=rows)


def first_line(*command):
    return subprocess.check_output(command, text=True).splitlines()[0]


def collect(out, mode, pairs, quick, checks):
    qemu.OUT = out
    build = out / f"build-{mode}"
    names = ("sh", "bcheck", "bedges", "bfail", "lifejob") if mode == "check" else (
        "sh", "bopen", "bclose")
    if checks:
        names = ("sh", *checks, "lifejob")
    apps = {name: build / "apps" / f"{name}.bin" for name in names}
    args = ["make", "-s", "-j4", qemu.CONFIG, f"BUILD={build}", f"LIFECYCLE={mode}",
            "build", *map(str, apps.values())]
    subprocess.run(args, check=True)
    os.environ["MAKEFLAGS"] = f"BUILD={build} LIFECYCLE={mode}"
    artifact = dict(apps, kernel=build / "kernel.elf")
    data = dict(commit=first_line("git", "rev-parse", "HEAD"),
                source_clean=not subprocess.check_output(["git", "status", "--short"]).strip(),
                qemu=first_line("qemu-system-riscv64", "--version"),
                gcc=first_line("riscv64-linux-gnu-gcc", "--version"), build=args,
                timebase_hz=10_000_000, guest_memory_MiB=128, harts=2,
                hashes={name: hashlib.sha256(path.read_bytes()).hexdigest()
                        for name, path in artifact.items()}, boots=[], failures=[])
    nboots = pairs if mode == "time" else 1
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        for i in range(nboots):
            disk = Path(tmp) / f"{i:02d}.img"
            mkfs.make_image(disk, programs=list(apps.items()))
            os.environ["DISK"] = str(disk)
            vm = qemu.Machine(f"bounded-{mode}-{i:02d}")
            boot = dict(boot=i, warm=[], opened=[], closed=[], checks=[])
            data["boots"].append(boot)
            try:
                vm.until(b"seele$ ")
                if mode == "check":
                    if "bedges" in names:
                        for label in ("audit", "lifetime", "rotation"):
                            boot["checks"].append(sample(vm, f"bedges {label}", "EDGES PASS"))
                    if "bfail" in names:
                        for label in (3, 6, 7):
                            boot["checks"].append(sample(vm, f"bfail {label}", "FAILURE PASS"))
                    if "bcheck" in names:
                        for label in ("ref", "2", "4", "5", "6", "7"):
                            boot["checks"].append(sample(vm, f"bcheck {label}", "CHECK PASS"))
                elif mode == "time":
                    policies = (0, 3, 1, 6) if i % 2 == 0 else (6, 1, 3, 0)
                    scenarios = [(64, 512)] if quick else [(64, 512), (64, 1), (1, 512), (8, 512)]
                    for policy in policies:
                        boot["warm"].append(sample(vm, f"bopen {policy} 2 512", "OPEN PASS"))
                    for n, stride in scenarios:
                        for policy in policies:
                            boot["opened"].append(sample(vm, f"bopen {policy} {n} {stride}", "OPEN PASS"))
                    order = (2, 4, 6, 5)
                    order = tuple(order[(i + k) % 4] for k in range(4))
                    schedules = ("busy", "idle") if i % 2 == 0 else ("idle", "busy")
                    if quick:
                        schedules = ("busy",)
                    for schedule in schedules:
                        for policy in order:
                            boot["closed"].append(sample(vm, f"bclose {policy} {schedule}",
                                                         "CLOSE PASS"))
                else:
                    for n, stride in ((64, 512), (64, 1), (1, 512), (8, 512)):
                        for policy in (0, 3, 1, 6):
                            boot["opened"].append(sample(vm, f"bopen {policy} {n} {stride}", "OPEN PASS"))
                    for policy in (2, 4, 5, 6):
                        for variant in ("busy", "idle"):
                            boot["closed"].append(sample(vm, f"bclose {policy} {variant}", "CLOSE PASS"))
            except Exception as error:
                data["failures"].append(dict(boot=i, error=repr(error)))
                raise
            finally:
                vm.close()
                with (out / f"{mode}.json").open("w", encoding="utf-8") as file:
                    json.dump(data, file, separators=(",", ":"))
                    print(file=file)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("modes", nargs="+", choices=("check", "counts", "time"))
    parser.add_argument("--pairs", type=int, default=12)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--checks", nargs="+", choices=("bcheck", "bedges", "bfail"))
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error("--pairs must be positive")
    if args.checks and args.modes != ["check"]:
        parser.error("--checks is only supported with check mode")
    args.output.mkdir(parents=True, exist_ok=True)
    for mode in args.modes:
        collect(args.output.resolve(), mode, args.pairs, args.quick, args.checks)

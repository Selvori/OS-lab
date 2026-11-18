#!/usr/bin/env python3
"""Replay the template window retention, rollback and paired-boot experiments."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from bench_template import PRIMARY, sample
import mkfs
import test_lab4 as qemu

PIN_FIELDS = ("covered user_pages kernel_pages released_windows dropped_refs").split()
TOTAL_FIELDS = ("close scan_branches scan_slots release_ticks max_release_ticks "
                "user_peak kernel_peak user_before user_after kernel_before kernel_after "
                "mmap_before mmap_after procs_before procs_after files_before files_after").split()


def lines(vm, cmd):
    vm.send(cmd.encode() + b"\r")
    output = vm.until(b"seele$ ", timeout=150).decode()
    assert "PANIC" not in output, output
    return output


def pin(vm, policy):
    output = lines(vm, f"wpin {policy}")
    assert "WP PASS" in output and "USER FAULT" not in output, output
    values = {}
    for prefix, count in (("WP", len(PIN_FIELDS)), ("WT", len(TOTAL_FIELDS)), ("WE", 5)):
        rows = [list(map(int, line.split()[1:])) for line in output.splitlines()
                if line.startswith(prefix + " ") and line != "WP PASS"]
        assert all(len(row) == count for row in rows), output
        values[prefix] = rows
    assert len(values["WT"]) == len(values["WE"]) == 1, output
    points = [dict(zip(PIN_FIELDS, x)) for x in values["WP"]]
    assert [x["covered"] for x in points] == [0, 508, 509, 2047, 2048], output
    return dict(policy=policy, live=points, last_store_ticks=values["WE"][0],
                **dict(zip(TOTAL_FIELDS, values["WT"][0])))


def build(out, mode, programs):
    directory = Path("build") / ("window-" + mode)
    apps = {name: directory / "apps" / (name + ".bin") for name in programs}
    args = ["make", "-s", "-j4", qemu.CONFIG, f"BUILD={directory}", f"LIFECYCLE={mode}",
            "build", *map(str, apps.values())]
    subprocess.run(args, check=True)
    os.environ["MAKEFLAGS"] = f"BUILD={directory} LIFECYCLE={mode}"
    qemu.OUT = out

    def command(*words):
        return subprocess.check_output(words, text=True).splitlines()[0]
    data = dict(commit=command("git", "rev-parse", "HEAD"),
                source_clean=not subprocess.check_output(["git", "status", "--short"]).strip(),
                qemu=command("qemu-system-riscv64", "--version"),
                gcc=command("riscv64-linux-gnu-gcc", "--version"), timebase_hz=10_000_000,
                artifacts={k: hashlib.sha256(v.read_bytes()).hexdigest()
                           for k, v in dict(apps, kernel=directory / "kernel.elf").items()},
                boots=[], failures=[])
    return apps, data


def collect(out, mode, pairs):
    programs = ("sh", "wcheck", "tcheck", "lifejob") if mode == "check" else (
                "sh", "tbench", "wpin", "lifejob")
    apps, data = build(out, mode, programs)
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        for i in range(pairs if mode == "time" else 1):
            disk = Path(tmp) / f"guest-{i:02d}.img"
            mkfs.make_image(disk, programs=list(apps.items()))
            os.environ["DISK"] = str(disk)
            vm = qemu.Machine(f"window-{mode}-{i:02d}")
            order = (2, 3) if i % 2 == 0 else (3, 2)
            boot = dict(index=i, policy_order=order, warmup=[], primary=[], memory=[])
            data["boots"].append(boot)
            try:
                vm.until(b"seele$ ")
                if mode == "check":
                    output = lines(vm, "tcheck")
                    assert "TC PASS" in output, output
                    output = lines(vm, "wcheck")
                    assert "WC PASS" in output and "USER FAULT" not in output, output
                    boot["checks"] = [x[3:] for x in output.splitlines() if x.startswith("WC ")]
                else:
                    for policy in order:
                        boot["warmup"].append(sample(vm, PRIMARY, policy, 2))
                    for policy in order:
                        boot["primary"].append(sample(vm, PRIMARY, policy, 64))
                    for policy in order:
                        boot["memory"].append(pin(vm, policy - 1))
            except Exception as error:
                data["failures"].append(dict(boot=i, error=repr(error)))
                raise
            finally:
                try:
                    vm.close()
                finally:
                    (out / f"{mode}.json").write_text(json.dumps(data, indent=2) + "\n",
                                                      encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("modes", nargs="+", choices=("check", "counts", "time"))
    parser.add_argument("--pairs", type=int, default=12)
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error("--pairs must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    for mode in args.modes:
        collect(args.output.resolve(), mode, args.pairs)

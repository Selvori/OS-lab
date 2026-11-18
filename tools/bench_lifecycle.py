#!/usr/bin/env python3
"""Run the same disk-backed workload with timing-only and counter kernels."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile

import mkfs
import test_lab4 as qemu

FIELDS = (
    "elapsed fork_time reap_time copied exec_bytes user_alloc user_free user_peak "
    "kernel_alloc kernel_free kernel_peak table_alloc table_free pte_visits "
    "clone_slots destroy_slots cow_fault cow_copyout cow_copy cow_upgrade "
    "wait_sleep_time exec_heap_time heap_leaves leaf_checks leaf_reads upper_checks upper_reads heap_table_free "
    "user_left kernel_left nodes_left slots_used files_used"
).split()
BASE = [(pages, writes, 1, "fork") for pages in (0, 16, 256)
        for writes in ((0,) if not pages else (0, 4))]
BASE += [(256, 0, 1, "exec")]
WARM = [(pages, writes, stride, "fork")
        for pages in (16, 256, 2048)
        for writes, stride in ((0, 1), (4, 1), (pages, 1))]
WARM += [(2048, 4, 512, "fork"), (2048, 0, 1, "exec")]


def run_case(machine, case, runs):
    pages, writes, stride, mode = case
    cmd = f"lifebench {pages} {writes} {stride} {runs} {mode}"
    machine.send(cmd.encode() + b"\r")
    output = machine.until(b"seele$ ", timeout=60).decode()
    assert "LIFE PASS\r\n" in output and "USER FAULT" not in output and "PANIC" not in output, output
    lines = output.splitlines()
    batch = [int(x) for line in lines if line.startswith("LIFE BATCH ")
             for x in line.split()[2:]]
    rows = [[int(x) for x in line.split()[2:]] for line in lines if line.startswith("LIFE ROW ")]
    assert len(batch) == 18 and len(rows) == runs and all(len(row) == len(FIELDS) for row in rows), output
    checks = [int(x) for line in lines if line.startswith("LIFE TABLES ")
              for x in line.split()[2:]]
    assert len(checks) == 18 and sum(checks) == rows[-1][FIELDS.index("leaf_checks")], output
    return dict(last_round_tables=dict(reap=checks[:9], exec=checks[9:]),
                init=batch[0], batch=batch[1], time_read=batch[2:],
                rounds=[dict(zip(FIELDS, row)) for row in rows])


def command(*args):
    return subprocess.check_output(args, cwd=qemu.ROOT, text=True).strip()


def build(mode):
    directory = Path("build") / f"lifecycle-{mode}"
    targets = {name: directory / "apps" / f"{name}.bin" for name in ("sh", "lifebench", "lifejob")}
    subprocess.run(["make", "-B", "-j4", qemu.CONFIG, f"BUILD={directory}", f"LIFECYCLE={mode}",
                    "build", *map(str, targets.values())], cwd=qemu.ROOT, check=True)
    return directory, list(targets.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("base", "warm"), default="base")
    parser.add_argument("--runs", type=int, choices=range(1, 16), default=12)
    parser.add_argument("--batches", type=int, default=3)
    parser.add_argument("--variant", choices=("standard-cow", "batch-unmap"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.batches < 1:
        parser.error("--batches must be positive")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = dict(schema_version=2, variant=args.variant, artifacts={},
                  platform="QEMU virt, OpenSBI default, 128 MiB, 2 harts",
                  commit=command("git", "rev-parse", "HEAD"),
                  dirty=bool(command("git", "status", "--porcelain", "--untracked-files=no")),
                  suite=args.suite, runs=args.runs, batches=args.batches, warmup_runs=2,
                  qemu=command("qemu-system-riscv64", "--version").splitlines()[0],
                  compiler=command("riscv64-linux-gnu-gcc", "--version").splitlines()[0],
                  host=command("uname", "-srmo"), timebase_hz=10_000_000,
                  kernel_flags=Path("mk/common.mk").read_text().split("CFLAGS := ")[1].splitlines()[0],
                  cases=[], failures=[])

    try:
        for mode in ("time", "counts"):
            directory, programs = build(mode)
            files = dict(programs)
            files["kernel.elf"] = directory / "kernel.elf"
            result["artifacts"][mode] = {
                name: hashlib.sha256((qemu.ROOT / path).read_bytes()).hexdigest()
                for name, path in files.items()
            }
            with tempfile.TemporaryDirectory(prefix="lifecycle-", dir=args.output.parent) as tmp:
                image = Path(tmp).resolve() / "bench.img"
                mkfs.make_image(image, programs=[(name, qemu.ROOT / path) for name, path in programs])
                env = {name: os.environ.get(name) for name in ("MAKEFLAGS", "DISK")}
                os.environ["MAKEFLAGS"] = f"BUILD={directory} LIFECYCLE={mode}"
                os.environ["DISK"] = str(image)
                qemu.OUT = args.output.parent
                machine = None
                try:
                    machine = qemu.Machine(args.output.stem + "-" + mode)
                    machine.until(b"seele$ ")
                    for case in BASE if args.suite == "base" else WARM:
                        pages, writes, stride, job = case
                        entry = dict(mode=mode, pages=pages, writes=writes, stride=stride,
                                     job=job, samples=[])
                        result["cases"].append(entry)
                        entry["warmup"] = run_case(machine, case, 2)
                        for _ in range(args.batches):
                            entry["samples"].append(run_case(machine, case, args.runs))
                        rounds = [row for sample in entry["samples"] for row in sample["rounds"]]
                        entry["median"] = {
                            field: statistics.median(row[field] for row in rounds) for field in FIELDS
                        }
                finally:
                    try:
                        if machine is not None:
                            machine.close()
                    finally:
                        for name, value in env.items():
                            if value is None:
                                os.environ.pop(name, None)
                            else:
                                os.environ[name] = value
    except Exception as error:
        result["failures"].append(repr(error))
        raise
    finally:
        args.output.write_text(json.dumps(result, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()

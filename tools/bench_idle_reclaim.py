#!/usr/bin/env python3
"""Collect independent QEMU boots for the idle-drain policies and summarize raw ticks."""

import argparse
import gzip
import hashlib
from itertools import permutations
import json
import os
from pathlib import Path
from random import Random
from statistics import median, quantiles
import subprocess
import tempfile

import mkfs
import test_lab4 as qemu
from bench_bounded_reclaim import sample

POLICIES = (1, 6, 8)
TERMINAL_POLICIES = (1, 8, 9)
TIMEBASE_HZ = 10_000_000
COMMANDS = (
    ("byield 6", "YIELD PASS"),
    ("byield 8", "YIELD PASS"),
    ("bcheck 1", "CHECK PASS"),
    ("bcheck 6", "CHECK PASS"),
    ("bcheck 7", "CHECK PASS"),
    ("bcheck 8", "CHECK PASS"),
    ("bcheck ref", "CHECK PASS"),
    ("byield ref1", "OWNER PASS"),
    ("byield ref6", "OWNER PASS"),
    ("byield ref8", "OWNER PASS"),
)
TERMINAL_COMMANDS = (
    ("byield 9", "YIELD PASS"),
    ("bcheck 9", "CHECK PASS"),
    ("byield ref9", "OWNER PASS"),
    ("byield sparse9", "SPARSE PASS"),
)


def first_line(*command):
    return subprocess.check_output(command, text=True).splitlines()[0]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_mode(out, mode):
    build = out / f"build-{mode}"
    names = ("sh", "byield", "bcheck") if mode == "check" else ("sh", "bopen")
    apps = {name: build / "apps" / f"{name}.bin" for name in names}
    cmd = ["make", "-s", "-j4", qemu.CONFIG, f"BUILD={build}", f"LIFECYCLE={mode}",
           "build", *map(str, apps.values())]
    with (out / f"{mode}-build.log").open("wb") as log:
        subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
    return dict(build=build, apps=apps, command=cmd)


def open_case(vm, policy, tasks, mode):
    case = sample(vm, f"bopen {policy} {tasks} 512", "OPEN PASS")
    rows = case["rows"]
    assert rows["OPEN"][0][:3] == [policy, tasks, 512]
    assert [r[0] for r in rows["ROUND"]] == list(range(tasks))
    if mode == "counts":
        assert rows["REF"][0][:2] == [0, 0]
        assert rows["FIX"][0][4] == 1
        if policy in (6, 8, 9):
            assert rows["REF"][0][4:10] == [2048, 0, 0, 1, 2048, 0]
            batches, busy, idle, confirm_max, release_max, confirmed, *_ = rows["SCH"][0]
            assert batches == busy + idle == 256
            assert release_max == 8
            assert (confirm_max, confirmed) == ((0, 0) if policy == 9 else (32, 33))
            assert (idle == 0 if policy == 6 else idle > 0)
    return case


def boot(out, mode, index, order, built, commands=COMMANDS):
    os.environ["MAKEFLAGS"] = f"BUILD={built['build']} LIFECYCLE={mode}"
    label = f"{mode}-{index:02d}"
    sd = dict(boot=index, order=order)
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        disk = Path(tmp) / "guest.img"
        mkfs.make_image(disk, programs=list(built["apps"].items()))
        os.environ["DISK"] = str(disk)
        vm = qemu.Machine(label)
        try:
            vm.until(b"seele$ ")
            if mode == "check":
                sd["checks"] = [sample(vm, command, success) for command, success in commands]
            else:
                sd["warm"] = [open_case(vm, p, 2, mode) for p in order]
                sd["samples"] = [open_case(vm, p, 64, mode) for p in order]
        finally:
            vm.close()
        assert b"PANIC" not in vm.log
    sd["serial_sha256"] = digest(out / f"{label}-serial.log")
    return sd


def save(path, value):
    path.write_bytes(gzip.compress((json.dumps(value, separators=(",", ":")) + "\n").encode(),
                                   mtime=0))


def collect(out, quick, checks_only, terminal):
    out.mkdir(parents=True, exist_ok=True)
    qemu.OUT = out
    data = dict(commit=first_line("git", "rev-parse", "HEAD"),
                source_clean=not subprocess.check_output(["git", "status", "--porcelain"]).strip(),
                qemu=first_line("qemu-system-riscv64", "--version"),
                gcc=first_line("riscv64-linux-gnu-gcc", "--version"),
                harts=2, memory_mib=128, timebase_hz=TIMEBASE_HZ, modes={})
    modes = ("check",) if checks_only else ("check", "time", "counts")
    policies = TERMINAL_POLICIES if terminal else POLICIES
    orders = list(permutations(policies))
    plan = {"check": [None] * 6, "time": orders * (4 if terminal else 2), "counts": orders}
    commands = COMMANDS + TERMINAL_COMMANDS if terminal else COMMANDS
    if terminal:
        data["policies"] = policies
    builds = {mode: build_mode(out, mode) for mode in modes}
    for mode, built in builds.items():
        files = dict(built["apps"], kernel=built["build"] / "kernel.elf")
        group = dict(build=built["command"], hashes={n: digest(p) for n, p in files.items()},
                     boots=[])
        data["modes"][mode] = group
        for index, order in enumerate(plan[mode][:1] if quick else plan[mode]):
            group["boots"].append(boot(out, mode, index, order, built, commands))
            print(f"{mode} boot {index + 1}: {order or 'guest checks'}", flush=True)
            save(out / "raw.json.gz", data)
    if not checks_only:
        for app in ("sh", "bopen"):
            assert data["modes"]["time"]["hashes"][app] == data["modes"]["counts"]["hashes"][app]


def interval(values):
    rng = Random(20261121)
    estimates = [median(rng.choices(values, k=len(values))) for _ in range(20000)]
    cuts = quantiles(estimates, n=40, method="inclusive")
    return dict(median=median(values), ci95=[cuts[0], cuts[-1]])


def summarize(data):
    policies = tuple(data.get('policies', POLICIES))
    results = {}
    rows = []
    ticks_us = data["timebase_hz"] / 1e6
    for entry in data["modes"]["time"]["boots"]:
        row = {}
        for case in entry["samples"]:
            policy, n, stride, setup, spawn, close, drain, total, *_ = case["rows"]["OPEN"][0]
            assert (n, stride) == (64, 512)
            row[policy] = dict(freeze_us=setup / ticks_us, spawn_us=spawn / ticks_us / n,
                               close_us=close / ticks_us, drain_us=drain / ticks_us,
                               total_us=total / ticks_us / n)
        assert set(row) == set(policies)
        rows.append(row)
    for p in policies:
        results[p] = {name: median(row[p][name] for row in rows) for name in rows[0][p]}
    comparisons = {}
    terminal = policies == TERMINAL_POLICIES
    pairs_to_compare = ((9, 8), (9, 1), (8, 1)) if terminal else ((6, 1), (8, 6), (8, 1))
    metrics = ("total_us", "drain_us", "close_us", "spawn_us") if terminal else ("total_us", "drain_us")
    for candidate, control in pairs_to_compare:
        comparison = {}
        for metric in metrics:
            pairs = [(row[candidate][metric], row[control][metric]) for row in rows]
            comparison[metric] = dict(delta=interval([a - b for a, b in pairs]),
                                      pct=interval([100 * (a / b - 1) for a, b in pairs]))
        comparisons[f"{candidate}/{control}"] = comparison
    return dict(medians=results, paired=comparisons, time_boots=rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--quick", action="store_true")
    mode.add_argument("--summarize", action="store_true")
    mode.add_argument("--checks-only", action="store_true")
    parser.add_argument("--terminal", action="store_true",
                        help="compare 1/8/9 with 24 time boots and terminal frame checks")
    args = parser.parse_args()
    if not args.summarize:
        collect(args.out.resolve(), args.quick, args.checks_only, args.terminal)
    if args.checks_only:
        raise SystemExit(0)
    with gzip.open(args.out / "raw.json.gz", "rt", encoding="utf-8") as raw:
        print(json.dumps(summarize(json.load(raw)), indent=2))

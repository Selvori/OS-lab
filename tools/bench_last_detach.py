#!/usr/bin/env python3
"""Paired timings for releasing a closed template after its last borrower leaves."""
import argparse
import json
from pathlib import Path
import shutil
from statistics import median

import bench_template_dual_hart as bench

WORKLOADS = ((9, 1), (9, 2_000_000), (9, 128_000_000), (1, 2_000_000))
TICKS_PER_MS = 10_000


def measure(machine, case, zombie):
    if zombie:
        output = bench.lines(machine, "latewait wait")
        assert "DETACH PASS" in output and "FAIL " not in output, output
        (first, last, tail, pending), = bench.read_rows(output, "TIMED WAIT", 4)
        assert pending == 3, output
        return {"first_wait": first, "last_wait": last, "drain": tail}
    policy, iterations = case
    result = bench.result(bench.lines(machine, f"tdbench {policy} {iterations}"), policy, iterations)
    ticks = result["ticks"]
    ticks["through_drain"] = ticks["both_waited"] + ticks["post_wait_drain"]
    return result


def summarize(boots, workloads, zombie):
    result = []
    for case in workloads:
        fields = ("first_wait", "last_wait", "drain") if zombie else (
            "both_waited", "post_wait_drain", "through_drain", "spawn1", "spawn2", "close")
        for field in fields:
            by_build = {"old": [], "new": []}
            for boot in boots:
                values = [row["result"] for row in boot["measured"] if row["case"] == case]
                ticks = [row[field] if zombie else row["ticks"][field] for row in values]
                assert len(ticks) == 3
                by_build[boot["build"]].append(median(ticks))
            old, new = by_build["old"], by_build["new"]
            ratios = [100 * (b / a - 1) for a, b in zip(old, new)]
            result.append(dict(case=case, metric=field,
                               old_ms=median(old) / TICKS_PER_MS, new_ms=median(new) / TICKS_PER_MS,
                               paired_percent=median(ratios), range_percent=[min(ratios), max(ratios)]))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "candidate", "disk", "output"):
        ap.add_argument(name, type=Path)
    ap.add_argument("--zombies", action="store_true", help="use two LIFECYCLE=check kernels and the latewait guest with already exited children")
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    paths = dict(old=args.baseline, new=args.candidate, disk=args.disk)
    digests = {name: bench.digest(path) for name, path in paths.items()}
    assert digests["old"] != digests["new"], "identical kernel artifacts"
    workloads = ("zombies",) if args.zombies else WORKLOADS
    boots = []
    for pair in range(4):
        variants = ("old", "new") if pair % 2 == 0 else ("new", "old")
        offset = pair % len(workloads)
        cases = workloads[offset:] + workloads[:offset]
        for variant in variants:
            label = f"pair-{pair+1}-{variant}"
            assert all(bench.digest(paths[name]) == value for name, value in digests.items())
            disk = args.disk
            if args.zombies:
                disk = args.output / (label + ".img")
                shutil.copyfile(args.disk, disk)
            machine = bench.boot(args.output, label, paths[variant], disk)
            try:
                owner = bench.boot_owner(machine)
                warmup = [dict(case=case, result=measure(machine, case, args.zombies)) for case in cases]
                samples = []
                for _ in range(3):
                    for case in cases:
                        samples.append(dict(case=case, result=measure(machine, case, args.zombies)))
            finally:
                machine.close()
                if args.zombies:
                    disk.unlink()
            assert all(bench.digest(paths[name]) == value for name, value in digests.items())
            boots.append(dict(pair=pair+1, build=variant, owner=owner, warmup=warmup, measured=samples))
    report = dict(sha256=digests, timebase_hz=10_000_000, boots=boots,
                  summary=summarize(boots, workloads, args.zombies))
    (args.output / "paired.json").write_text(json.dumps(report, indent=2) + "\n")
    for row in report["summary"]:
        if row["metric"] not in ("spawn1", "spawn2", "close"):
            print(row)


if __name__ == "__main__":
    main()

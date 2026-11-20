#!/usr/bin/env python3
"""Tabulate per-task and shutdown medians and paired differences against freeze(1)."""
import argparse
import gzip
import json
from pathlib import Path
from random import Random
from statistics import median, quantiles

def summarize(data):
    times = data["modes"]["time"]
    counts = data["modes"]["counts"]
    assert len(times["boots"]) == 14 and len(counts["boots"]) == 1
    assert all(times["hashes"][name] == counts["hashes"][name] for name in ("sh","bopen"))
    by_policy = {i:[] for i in range(7)}
    cycle_pairs = {i:[] for i in range(7)}
    for boot in times["boots"]:
        batch = {}
        for item in boot["samples"]:
            p,n,stride,setup,spawn,close,drain,total = item["rows"]["OPEN"][0][:8]
            assert (n,stride) == (64,512)
            assert len(item["rows"]["ROUND"]) == 64
            ticks_per_us = data["timebase_hz"] / 1e6
            by_policy[p].append(dict(spawn_us=spawn/ticks_per_us/n,
                                     before_close_us=(total-close-drain)/ticks_per_us/n,
                                     close_us=close/ticks_per_us,drain_us=drain/ticks_per_us,
                                     total_us=total/ticks_per_us/n))
            batch[p] = total
        assert set(batch) == set(range(7))
        for p in batch:
            cycle_pairs[p].append(100*(batch[p]/batch[1]-1))
    result = {}
    for item in counts["boots"][0]["samples"]:
        rows = item["rows"]
        p, n = rows["OPEN"][0][:2]
        count = rows["REF"][0]
        samples = by_policy[p]
        values = {k:median(x[k] for x in samples) for k in samples[0]}
        diffs = cycle_pairs[p]
        rng = Random(20261120)
        estimates = [median(rng.choices(diffs,k=len(diffs))) for _ in range(20000)]
        cuts = quantiles(estimates,n=40,method="inclusive")
        result[p] = dict(**values, paired_total_pct=median(diffs), ci95_pct=[cuts[0],cuts[-1]],
                         borrow_retains=count[0],retain_locks=count[1],drop_locks=count[3],
                         copied_slots=rows["OPEN"][0][9])
        assert n==64 and len(samples)==14
    return result

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw",type=Path)
    args = parser.parse_args()
    with gzip.open(args.raw,"rt",encoding="utf-8") as f:
        data = json.load(f)
    print(json.dumps(summarize(data),indent=2,sort_keys=True))

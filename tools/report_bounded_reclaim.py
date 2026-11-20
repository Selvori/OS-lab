#!/usr/bin/env python3
"""Recompute paired statistics and late-reclaim timing from the raw QEMU samples."""
import argparse
import csv
import gzip
import json
from pathlib import Path
import random
from statistics import median


def load(directory, name):
    path = directory / f"{name}.json"
    if path.exists():
        return json.loads(path.read_bytes())
    with gzip.open(path.with_suffix(".json.gz"), "rt", encoding="utf-8") as file:
        return json.load(file)


def percentile(values, q):
    values = sorted(values)
    i = (len(values) - 1) * q
    lo = int(i)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (i - lo) * (values[hi] - values[lo])


def distribution(values):
    return {name: round(percentile(values, q), 4) for name, q in
            (("p50", 0.5), ("p95", 0.95), ("p99", 0.99), ("max", 1))}


def ci(relative):
    rng = random.Random(20261120)
    draws = [median(rng.choices(relative, k=len(relative))) for _ in range(20_000)]
    return dict(paired_median_pct=round(median(relative), 4),
                ci95_pct=[round(percentile(draws, q), 4) for q in (0.025, 0.975)])


def summarise_open(time, counts):
    rows, summary = [], []
    for boot in time["boots"]:
        for sample in boot["opened"]:
            policy, n, stride, setup, spawn, close, drain, total, reap, *_ = sample["rows"]["OPEN"][0]
            rows.append(dict(boot=boot["boot"], policy=policy, tasks=n, stride=stride,
                             freeze_us=setup / 10, spawn_us_per_task=spawn / (10 * n),
                             reap_us_per_task=reap / (10 * n), close_us=close / 10,
                             drain_us=drain / 10, pre_close_us_per_task=(total - close - drain) / (10 * n),
                             amortized_us_per_task=total / (10 * n)))
    metrics = ("freeze_us", "spawn_us_per_task", "reap_us_per_task", "close_us",
               "drain_us", "pre_close_us_per_task", "amortized_us_per_task")
    policies = (0, 3, 1, 6)
    for n, stride in sorted({(row["tasks"], row["stride"]) for row in rows}):
        series = {policy: {r["boot"]: r for r in rows
                           if (r["tasks"], r["stride"], r["policy"]) == (n, stride, policy)}
                  for policy in policies}
        assert all(x.keys() == series[1].keys() for x in series.values())
        group = dict(tasks=n, stride=stride, pairs=len(series[1]), policies={}, contrasts=[])
        for policy in policies:
            group["policies"][policy] = {
                key: distribution([r[key] for r in series[policy].values()]) for key in metrics
            }
        for lhs, rhs in ((3, 0), (1, 3), (6, 1)):
            for key in ("spawn_us_per_task", "pre_close_us_per_task", "amortized_us_per_task"):
                relative = [(series[lhs][b][key] / series[rhs][b][key] - 1) * 100
                            for b in series[rhs]]
                group["contrasts"].append(dict(policy=lhs, control=rhs, metric=key, **ci(relative)))
        summary.append(group)

    control_counts = []
    for sample in counts["boots"][0]["opened"]:
        r = sample["rows"]
        policy, n, stride, *_, slots, _, kernel_peak = r["OPEN"][0]
        retain, retain_locks, drop, drop_locks, template_drops, returned, pending, *_ = r["REF"][0]
        assert not pending
        if policy in (1, 6):
            assert retain == retain_locks == 0
        control_counts.append(dict(policy=policy, tasks=n, stride=stride, copied_slots=slots,
                                   retain=retain, retain_locks=retain_locks, drop=drop,
                                   drop_locks=drop_locks, template_drops=template_drops,
                                   returned=returned, kernel_peak=kernel_peak))
    return rows, summary, control_counts


def validate_frames(series, close, quiet):
    assert len(series) == 2048 and len({r[1] for r in series}) == 2048
    for i, (index, frame, borrow, drop, returned) in enumerate(series):
        assert index == i and frame % 4096 == 0 and close <= borrow <= drop <= returned <= quiet


def summarise_closed(time, counts):
    rows, summary = [], {}
    windows = (508, 1020, 1532, 2044, 2047)
    raw_groups = {(idle, policy): {key: [] for key in ("store", "frame", "batch", "gap")}
                  for idle in (0, 1) for policy in (2, 4, 5, 6)}
    for mode, data in (("time", time), ("counts", counts)):
        for boot in data["boots"]:
            for sample in boot["closed"]:
                r = sample["rows"]
                policy, idle, start, close, quiet, _, peak, kernel_peak, live, initial, kernel_initial = r["CLOSE"][0]
                validate_frames(r["FRAME"], close, quiet)
                written = r["WRITTEN"][0][0]
                assert close <= written <= quiet
                slots = r["STORE"]
                assert [i for i, _ in slots] == list(range(2048))
                batch_samples = r.get("BATCH", [])
                for i, batch in enumerate(batch_samples):
                    index, _, confirmed, released, batch_idle, finalized = batch
                    assert index == i and confirmed <= 32 and released <= 8 and finalized <= 1
                    assert batch_idle in (0, 1)
                if policy != 2:
                    assert sum(b[3] for b in batch_samples) == 2048
                    assert sum(b[5] for b in batch_samples) == 1
                else:
                    assert not batch_samples
                stores = [ticks / 10 for _, ticks in slots]
                frame_lags = [(returned - borrow) / 10 for _, _, borrow, _, returned in r["FRAME"]]
                batch_ticks = [ticks / 10 for _, ticks, *_ in batch_samples]
                gaps = []
                if not idle:
                    progress = [ticks for _, ticks, _ in r["PROGRESS"]]
                    assert 1 < len(progress) < 4096 and progress[0] <= start <= quiet <= progress[-1]
                    assert all(a <= b for a, b in zip(progress, progress[1:]))
                    gaps = [(b - a) / 10 for a, b in zip(progress, progress[1:])
                            if b >= start and a <= quiet]
                _, _, fixed, _, _, tables, kernel, user = r["FIX"][0]
                raw = dict(store=stores, frame=frame_lags, batch=batch_ticks, gap=gaps)
                row = dict(mode=mode, boot=boot["boot"], policy=policy, idle=idle,
                           close_us=(close - start) / 10,
                           close_to_quiet_us=(quiet - start) / 10,
                           quiet_after_write_us=(quiet - written) / 10,
                           last_return_after_borrow_us=(max(x[4] for x in r["FRAME"]) -
                                                        max(x[2] for x in r["FRAME"])) / 10,
                           last_return_after_write_us=max(0, max(x[4] for x in r["FRAME"]) - written) / 10,
                           observed_peak_user_pages=initial + peak if mode == "counts" else None,
                           observed_peak_kernel_pages=kernel_initial + kernel_peak if mode == "counts" else None,
                           live_user_pages=live, fixed_us=fixed / 10, fixed_tables=tables,
                           fixed_kernel_pages=kernel, fixed_user_pages=user,
                           busy_batches=sum(not b[4] for b in batch_samples),
                           idle_batches=sum(b[4] for b in batch_samples))
                for window, last in enumerate(windows):
                    row[f"window_{window}_last_store_us"] = stores[last]
                slowest = sorted(slots, key=lambda x: x[1], reverse=True)[:5]
                row["slowest_stores"] = ";".join(f"{i}={ticks / 10:.1f}" for i, ticks in slowest)
                for key, values in raw.items():
                    values_summary = distribution(values) if values else {k: None for k in ("p50", "p95", "p99", "max")}
                    for q, value in values_summary.items():
                        row[f"{key}_{q}_us"] = value
                    if mode == "time":
                        raw_groups[idle, policy][key].extend(values)
                rows.append(row)
    metric_names = ("close_us", "close_to_quiet_us", "quiet_after_write_us", "last_return_after_borrow_us",
                    "fixed_us", "busy_batches", "idle_batches", "gap_max_us", "store_max_us",
                    "frame_max_us", "batch_max_us", *(f"window_{i}_last_store_us" for i in range(5)))
    for (idle, policy), raw in raw_groups.items():
        samples = [r for r in rows if r["mode"] == "time" and r["policy"] == policy and r["idle"] == idle]
        summary[f"{'idle' if idle else 'busy'}:{policy}"] = {
            "runs": len(samples),
            "across_runs": {k: distribution([row[k] for row in samples]) for k in metric_names
                            if samples[0][k] is not None},
            "pooled": {k: distribution(values) for k, values in raw.items() if values},
        }
    return rows, summary


def write_csv(path, rows):
    with path.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, rows[0].keys(), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    check, counts, time = (load(args.samples, name) for name in ("check", "counts", "time"))
    assert all(not d["failures"] for d in (check, counts, time))
    assert len({d["commit"] for d in (check, counts, time)}) == 1
    assert all(counts["hashes"][name] == time["hashes"][name] for name in counts["hashes"] if name != "kernel")
    opened, open_summary, open_counts = summarise_open(time, counts)
    closed, close_summary = summarise_closed(time, counts)
    write_csv(args.output / "open-pairs.csv", opened)
    write_csv(args.output / "open-counts.csv", open_counts)
    write_csv(args.output / "close-samples.csv", closed)
    summary = dict(commit=time["commit"], same_apps=True, open=open_summary, close=close_summary)
    with (args.output / "summary.json").open("w", encoding="utf-8", newline="\n") as file:
        print(json.dumps(summary, indent=2), file=file)


if __name__ == "__main__":
    main()

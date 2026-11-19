"""Verify the three identical-input warm suites and emit timing and count summaries."""
import csv
import json
from pathlib import Path
import statistics
import sys

root = Path(sys.argv[1])
runs = ("baseline", "candidate", "baseline-repeat")
datasets = {name: json.loads((root / "raw" / f"{name}.json").read_text()) for name in runs}
case_fields = ("mode", "pages", "writes", "stride", "job")
expected_jobs = [(pages, writes, 1, "fork") for pages in (16, 256, 2048)
                 for writes in (0, 4, pages)] + [(2048, 4, 512, "fork"), (2048, 0, 1, "exec")]
fields = ("cow_fault", "cow_copyout", "cow_copy", "cow_upgrade", "copied",
          "user_alloc", "user_free", "user_peak", "kernel_peak", "user_left",
          "kernel_left", "nodes_left", "slots_used", "files_used")
resource_fields = ("user_left", "kernel_left", "nodes_left", "slots_used", "files_used")
balances = {}
timings = []
counts = []

for name, d in datasets.items():
    assert not d["dirty"] and not d["failures"] and d["timebase_hz"] == 10_000_000
    assert (d["suite"], d["warmup_runs"], d["batches"], d["runs"]) == ("warm", 2, 3, 12)
    assert [tuple(c[k] for k in case_fields) for c in d["cases"]] == [
        (mode, *job) for mode in ("time", "counts") for job in expected_jobs
    ]
    for c in d["cases"]:
        assert len(c["samples"]) == 3 and all(len(b["rounds"]) == 12 for b in c["samples"])
        assert len(c["warmup"]["rounds"]) == 2
        rows = [r for b in c["samples"] for r in b["rounds"]]
        mode = c["mode"]
        job = tuple(c[k] for k in case_fields[1:])
        pages, writes, stride, operation = job
        balance = {tuple(r[k] for k in resource_fields) for r in rows + c["warmup"]["rounds"]}
        assert len(balance) == 1
        assert balance == balances.setdefault((mode, job), balance)
        if mode == "time":
            assert all(not row["cow_fault"] and not row["user_alloc"] for row in rows)
            elapsed = [r["elapsed"] for r in rows]
            p10, *_, p90 = statistics.quantiles(elapsed, n=10, method="inclusive")
            batch_medians = [statistics.median(r["elapsed"] for r in b["rounds"]) for b in c["samples"]]
            timing = [statistics.median(elapsed), p10, p90, *batch_medians]
            phases = [statistics.median(r[k] for r in rows)
                      for k in ("fork_time", "wait_sleep_time", "reap_time")]
            timings.append([name, *job, *(round(t / 10_000, 6) for t in timing + phases)])
            continue
        values = {tuple(r[k] for k in fields) for r in rows + c["warmup"]["rounds"]}
        assert len(values) == 1, (name, job, values)
        row = dict(zip(fields, next(iter(values))))
        fault, copyout, copies, upgrades = (row[k] for k in fields[:4])
        # All measured rounds succeeded. Each demand fault and copyout contributes
        # one commit; the remaining commits are successful speculative copies.
        prefetched = copies + upgrades - fault - copyout
        assert copyout == 1 and prefetched >= 0
        assert row["user_alloc"] == row["user_free"] and row["copied"] == 4096 * copies
        unused = 0
        if operation == "fork":
            assert upgrades == 0 and 0 <= writes - fault <= prefetched
            unused = prefetched - (writes - fault)
            assert copies == copyout + writes + unused
        else:
            assert fault == 1 and prefetched == 0
        if name != "candidate":
            assert prefetched == 0
        counts.append([name, *job, *(row[k] for k in fields), prefetched, unused])

hashes = {}
for basename in ("sh", "lifebench", "lifejob"):
    found = {d["artifacts"][mode][basename] for d in datasets.values() for mode in ("time", "counts")}
    assert len(found) == 1
    hashes[basename] = found.pop()
assert datasets["baseline"]["artifacts"] == datasets["baseline-repeat"]["artifacts"]

for filename, columns, rows in (
    ("timings.csv", ("run", *case_fields[1:], "median_ms", "p10_ms", "p90_ms", "batch1_ms",
                     "batch2_ms", "batch3_ms", "fork_ms", "wait_sleep_ms", "reap_ms"), timings),
    ("counts.csv", ("run", *case_fields[1:], *fields, "prefetched", "unused_prefetch"), counts),
):
    with (root / filename).open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp, lineterminator="\n")
        writer.writerow(columns)
        writer.writerows(rows)

print(json.dumps(hashes, indent=2))
print(f"{sum(len(d['cases']) for d in datasets.values()) * 36} measured rounds, "
      "same per-mode resource balances and identical userspace artifacts")

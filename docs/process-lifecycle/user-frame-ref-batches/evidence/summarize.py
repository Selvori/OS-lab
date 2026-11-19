import csv
import json
from pathlib import Path
from statistics import median, quantiles
import sys

directory = Path(sys.argv[1])
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
versions = ("baseline", "candidate", "baseline-repeat")
data = [json.loads((directory / (v + ".json")).read_text()) for v in versions]
names = ("sh", "lifebench", "lifejob")
assert all(not d["dirty"] and not d["failures"] and d["suite"] == "warm" for d in data)
assert data[0]["commit"] == data[2]["commit"] != data[1]["commit"]
assert data[0]["artifacts"] == data[2]["artifacts"]
assert all(d["artifacts"][m][n] == data[0]["artifacts"]["time"][n]
           for d in data for m in ("time", "counts") for n in names)
assert all(d[k] == data[0][k] for d in data
           for k in ("compiler", "kernel_flags", "qemu", "timebase_hz", "host", "platform"))
assert all((d["runs"], d["batches"], d["warmup_runs"], len(d["cases"])) == (12, 3, 2, 22) for d in data)

labels = ("64 KiB 只读", "64 KiB 连写 4 页", "64 KiB 全写",
          "1 MiB 只读", "1 MiB 连写 4 页", "1 MiB 全写",
          "8 MiB 只读", "8 MiB 连写 4 页", "8 MiB 全写", "8 MiB 分散写 4 页", "8 MiB 即 exec")
phases = ("elapsed", "fork_time", "reap_time", "exec_heap_time", "wait_sleep_time")
stable_fields = [k for k in data[0]["cases"][11]["median"] if k not in phases]
def rows(case):
    return [r for s in case["samples"] for r in s["rounds"]]
def change(a, b):
    return None if a == 0 else b / a * 100 - 100

with (out / "timings.csv").open("w", newline="") as tm, (out / "counts.csv").open("w", newline="") as ct:
    timing = csv.writer(tm, lineterminator="\n")
    timing.writerow(["version", "pages", "writes", "stride", "job", "metric", "median_ticks",
                     "p10_ticks", "p90_ticks", "batch1_median_ticks", "batch2_median_ticks", "batch3_median_ticks"])
    counts = csv.writer(ct, lineterminator="\n")
    counts.writerow(["version", "pages", "writes", "stride", "job", *stable_fields])
    print("| 负载 | 整轮基线 → 候选 → 基线复测（ms） | 整轮变化 | fork 基线 → 候选（ms） | wait 销毁基线 → 候选（ms） |")
    print("| --- | ---: | ---: | ---: | ---: |")
    for index, group in enumerate(zip(*(d["cases"] for d in data))):
        key = tuple(group[0][k] for k in ("pages", "writes", "stride", "job"))
        assert all(tuple(c[k] for k in ("pages", "writes", "stride", "job")) == key for c in group)
        base, cand, repeat = group
        assert all(c["mode"] == base["mode"] for c in group)
        assert all(len(c["warmup"]["rounds"]) == 2 and
                   [len(s["rounds"]) for s in c["samples"]] == [12, 12, 12] for c in group)
        if base["mode"] == "time":
            for version, c in zip(versions, group):
                for phase in phases:
                    values = [r[phase] for r in rows(c)]
                    qs = quantiles(values, n=10, method="inclusive")
                    timing.writerow([version, *key, phase, median(values), qs[0], qs[-1],
                                     *(median(r[phase] for r in s["rounds"]) for s in c["samples"])])
            def med(key, cases=group):
                return " → ".join(f"{c['median'][key]/10_000:.5f}" for c in cases)
            print(f"| {labels[index]} | {med('elapsed')} | "
                  f"{change(base['median']['elapsed'], cand['median']['elapsed']):+.2f}% | "
                  f"{med('fork_time', group[:2])} | {med('reap_time', group[:2])} |")
        else:
            assert base["mode"] == "counts"
            for version, c in zip(versions, group):
                offset = 1 if version == "candidate" else 0
                vs = [row for sample in [c["warmup"], *c["samples"]] for row in sample["rounds"]]
                for r, b in zip(vs, [row for sample in [base["warmup"], *base["samples"]]
                                     for row in sample["rounds"]]):
                    assert all(r[k] + (offset if k == "user_left" else 0) == b[k] for k in stable_fields), (version, key, r, b)
                counts.writerow([version, *key, *(c["median"][k] for k in stable_fields)])
print()
print("Primary batches and percent changes")
for d in data:
    c = d["cases"][6]
    print(d["commit"], [median(r["elapsed"] for r in s["rounds"])/10000 for s in c["samples"]])
for i in range(11):
    c = [d["cases"][i] for d in data]
    f = [tuple(round(change(c[j]["median"][k], c[1]["median"][k]), 2) if c[j]["median"][k] else None
               for k in phases) for j in (0, 2)]
    slower = [[ri for ri,(x,y) in enumerate(zip(c[j]["samples"], c[1]["samples"]),1)
               if median(r["elapsed"] for r in y["rounds"]) >
                  median(r["elapsed"] for r in x["rounds"])] for j in (0, 2)]
    print(labels[i], f, slower)

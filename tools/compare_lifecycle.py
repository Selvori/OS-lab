#!/usr/bin/env python3
"""Compare matching raw lifecycle runs. Time columns are in milliseconds."""
import argparse
import json
import math
from pathlib import Path
from statistics import median


def load(path):
    result = json.loads(path.read_text())
    assert not result["failures"] and not result["dirty"], path
    scale = result["timebase_hz"] / 1000
    cases = {}
    for case in result["cases"]:
        key = tuple(case[name] for name in ("pages", "writes", "stride", "job"))
        samples = case["samples"]
        rows = [row for sample in samples for row in sample["rounds"]]
        assert len(samples) == result["batches"]
        assert all(len(batch["rounds"]) == result["runs"] for batch in samples)
        if result.get("schema_version") == 2:
            assert len(case["warmup"]["rounds"]) == result["warmup_runs"]
            for batch in [case["warmup"], *samples]:
                by_phase = batch["last_round_tables"]
                assert set(by_phase) == {"reap", "exec"}
                assert all(len(counts) == 9 for counts in by_phase.values())
                assert sum(sum(counts) for counts in by_phase.values()) == batch["rounds"][-1]["leaf_checks"]
        assert case["mode"] not in cases.get(key, {})
        balance = ("user_left", "kernel_left", "nodes_left", "slots_used", "files_used")
        assert len({tuple(row[field] for field in balance) for row in rows}) == 1
        if case["mode"] == "counts":
            for row in rows:
                assert row["user_alloc"] == row["user_free"]
                assert row["kernel_alloc"] == row["kernel_free"]
                assert row["table_alloc"] == row["table_free"]
            cases.setdefault(key, {})["counts"] = {k: median(row[k] for row in rows)
                                                  for k in rows[0]}
            continue
        elapsed = sorted(row["elapsed"] for row in rows)
        times = [median(elapsed), elapsed[math.ceil(0.10 * len(elapsed)) - 1],
                 elapsed[math.ceil(0.90 * len(elapsed)) - 1]]
        times.extend(median(row[name] for row in rows) for name in ("fork_time", "reap_time"))
        times.extend(median(sample[name] for sample in samples) for name in ("init", "batch"))
        target = cases.setdefault(key, {})
        target["time"] = [value / scale for value in times]
        if result.get("schema_version") == 2:
            target["phases"] = {
                name: median(row[name] for row in rows) / scale
                for name in ("wait_sleep_time", "exec_heap_time")
            }
    assert all("time" in c and "counts" in c for c in cases.values()), path
    return cases


def table(headers, rows):
    print("| " + " | ".join(headers) + " |")
    print("| " + " | ".join("---" for _ in headers) + " |")
    for row in rows:
        print("| " + " | ".join(row) + " |")


def label(case):
    pages, writes, stride, job = case
    return " / ".join((f"{pages * 4} KiB", job if job == "exec" else
                       f"{writes} 页写" + ("，分散" if stride > 1 else "")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("deep", type=Path)
    parser.add_argument("cow", type=Path)
    args = parser.parse_args()
    deep, cow = load(args.deep), load(args.cow)
    assert deep.keys() == cow.keys()

    def times(values):
        med, p10, p90, fork, reap, init, batch = values
        return (f"{med:.3f} [{p10:.3f}, {p90:.3f}]", f"{fork:.3f}/{reap:.3f}",
                f"{init:.3f}/{batch:.3f}")

    table(("工作集 / 任务", "深拷贝整轮 [P10, P90]", "COW 整轮 [P10, P90]",
           "深拷贝创建/回收", "COW 创建/回收", "深拷贝初始化/批次", "COW 初始化/批次"),
          [(label(k), times(deep[k]["time"])[0], times(cow[k]["time"])[0],
            times(deep[k]["time"])[1], times(cow[k]["time"])[1],
            times(deep[k]["time"])[2], times(cow[k]["time"])[2]) for k in deep])
    print()

    def pair(key, field):
        return f'{deep[key]["counts"][field]:.0f}/{cow[key]["counts"][field]:.0f}'

    table(("工作集 / 任务", "复制 B", "用户页峰值", "内核页峰值",
           "子表页", "PTE 探查", "COW 写缺页/代写"),
          [(label(k), *(pair(k, f) for f in ("copied", "user_peak", "kernel_peak",
                                              "table_alloc", "pte_visits")),
            f'{cow[k]["counts"]["cow_fault"]:.0f}/{cow[k]["counts"]["cow_copyout"]:.0f}')
           for k in deep])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Validate and summarize a closed template window experiment."""
import argparse
import json
import math
from pathlib import Path
import random
from statistics import median


def interval(values):
    rng = random.Random(20261118)
    distribution = sorted(median(rng.choices(values, k=len(values))) for _ in range(20000))
    return tuple(distribution[math.ceil(len(distribution) * p) - 1] for p in (.025, .975))


def report(folder):
    data = {name: json.loads((folder / f"{name}.json").read_text(encoding="utf-8"))
            for name in ("time", "counts", "check")}
    assert all(not run["failures"] for run in data.values())
    commits = {run["commit"] for run in data.values()}
    assert len(commits) == 1
    assert all(run["timebase_hz"] == 10_000_000 for run in data.values())
    assert all(data["counts"]["artifacts"][name] == data["time"]["artifacts"][name]
               for name in ("sh", "tbench", "wpin", "lifejob"))
    assert data["check"]["boots"][0]["checks"][-1] == "PASS"
    assert len(data["time"]["boots"]) >= 12
    pairs = []
    timings = {1: [], 2: []}
    for i, boot in enumerate(data["time"]["boots"]):
        order = [2, 3] if i % 2 == 0 else [3, 2]
        assert boot["index"] == i and boot["policy_order"] == order
        assert [row["policy"] for row in boot["primary"]] == order
        assert [row["policy"] for row in boot["warmup"]] == order
        assert all(row["case"] == [2048, 4, 512, "f"] and row["n"] == 2
                   for row in boot["warmup"])
        batch = sorted(boot["primary"], key=lambda row: row["policy"])
        assert all(row["case"] == [2048, 4, 512, "f"] and row["n"] == 64 and
                   len(row["rounds"]) == 64 and row["first_writes"] == 64 for row in batch)
        pairs.append(tuple(row["total"] / row["n"] / 10000 for row in batch))
        for row in boot["memory"]:
            timings[row["policy"]].append(row)
    absolute = [new - old for old, new in pairs]
    relative = [100 * (new - old) / old for old, new in pairs]
    low_ms, high_ms = interval(absolute)
    low_pct, high_pct = interval(relative)
    counts = sorted(data["counts"]["boots"][0]["memory"], key=lambda row: row["policy"])
    assert len(counts) == 2
    old, new = counts
    assert old["policy"] == 1 and new["policy"] == 2
    assert (new["scan_branches"], new["scan_slots"]) == (1, 2557)
    assert old["scan_branches"] == old["scan_slots"] == 0
    assert len(old["live"]) == len(new["live"]) == 5
    assert [row["dropped_refs"] for row in new["live"]] == [0, 0, 509, 2045, 2048]
    assert [a["user_pages"] - b["user_pages"] for a, b in zip(old["live"], new["live"])] == [
        0, 0, 509, 2045, 2048
    ]
    for row in counts + timings[1] + timings[2]:
        assert all(row[name + "_before"] == row[name + "_after"]
                   for name in ("user", "kernel", "mmap", "procs", "files"))
        assert [(p["user_pages"], p["kernel_pages"]) for p in row["live"]] == [
            (p["user_pages"], p["kernel_pages"]) for p in counts[row["policy"] - 1]["live"]
        ]
    primary_counts = sorted(data["counts"]["boots"][0]["primary"], key=lambda row: row["policy"])
    assert all(all(row[field] == primary_counts[0][field]
                   for field in ("copied_slots", "retains", "exit_slots", "bitmap_words",
                                 "private_drops", "user_peak", "kernel_peak"))
               for row in primary_counts)

    lines = ["# 关闭模板后的窗口提前回收结果", "", "## 开放期主场景", "",
             f"实测版本 {commits.pop()}。{len(pairs)} 次独立 QEMU 启动，交替执行顺序。"
             "每版同一 ELF，每次先预热 2 任务，再正式测 64 任务。8 MiB，分散写 4 页。"
             "摊销含冻结、派生、写入、wait 和最终关闭，窗口回收只会在关闭后仍有分支时启用。", "",
             f"- 旧模板 {median(a for a, _ in pairs):.4f} ms/任务，新策略 "
             f"{median(b for _, b in pairs):.4f} ms/任务。",
             f"- 新减旧的配对绝对差中位数 {median(absolute):+.5f} ms/任务，"
             f"95% 区间 [{low_ms:+.5f}, {high_ms:+.5f}] ms/任务。",
             f"- 新相对旧的配对差中位数 {median(relative):+.2f}%，95% 区间 "
             f"[{low_pct:+.2f}%, {high_pct:+.2f}%]。"
             f"按预设的 5% 上界预算{'达到' if high_pct <= 5 else '无法确认达到'}要求。",
             "- 两个区间以独立启动配对为单位，固定种子 20261118，自助抽样 20000 次。"
             "保留全部配对，区间用最近秩求经验分位数。", "",
             "| 开机 | 旧（ms/任务） | 新（ms/任务） | 相对差 |",
             "| ---: | ---: | ---: | ---: |"]
    for i, ((a, b), change) in enumerate(zip(pairs, relative), 1):
        lines.append(f"| {i} | {a:.4f} | {b:.4f} | {change:+.2f}% |")
    count = primary_counts[0]
    lines += ["", f"独立计数构建实测两版每任务同复制 {count['copied_slots'] / 64:g} 个堆表项，"
              f"逐帧增加模板分支引用 {count['retains']} 次，私有释放 {count['private_drops'] / 64:g} 页。"
              f"两版额外峰值均为 {count['user_peak']} 用户页和 {count['kernel_peak']} 内核页。", "",
              "## 关闭后分支仍存活", "",
              "5 个窗口分别有 509、512、512、512、3 个堆页。额外页以堆分配前为基准，"
              "包含 1 个两路共有的额外固定映像用户页。创建者关闭模板并撤去自己的原始映射后，分支才开始覆盖。", "",
              "| 已覆盖页 | 旧用户 / 内核 | 新用户 / 内核 | 新版累计释放窗口 / 原始引用 |",
              "| ---: | ---: | ---: | ---: |"]
    for a, b in zip(old["live"], new["live"]):
        assert a["covered"] == b["covered"] and a["kernel_pages"] == b["kernel_pages"]
        lines.append(f"| {a['covered']} | {a['user_pages']} / {a['kernel_pages']} | "
                     f"{b['user_pages']} / {b['kernel_pages']} | "
                     f"{b['released_windows']} / {b['dropped_refs']} |")
    lines += ["", f"瞬时峰值（用户 / 内核）为旧 {old['user_peak']} / {old['kernel_peak']} 页，"
              f"新 {new['user_peak']} / {new['kernel_peak']} 页。"
              "最终用户池、内核池、mmap 节点、进程槽及打开对象分别恢复，"
              f"本轮起始和结束值为 {new['user_before']} / {new['kernel_before']} / "
              f"{new['mmap_before']} / {new['procs_before']} / {new['files_before']}。", "",
              "## 关闭和末次覆盖成本", "",
              "一位分支仍存活时，计数版扫描 1 个分支的 2557 个 PTE 槽。"
              "下表取不包含页计数插桩的计时版中位数，单位 µs。末次写测单个用户 store 的"
              "时间戳差，其中包含缺页、复制、页回收以及返回。", "",
              "| 路径 | 关闭调用 | 窗口一末次写 | 窗口二末次写 | 窗口三末次写 | 窗口四末次写 | 窗口五末次写 |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for policy, label in ((1, "旧模板"), (2, "窗口回收")):
        rows = timings[policy]
        close = median(row["close"] for row in rows) / 10
        stores = [median(row["last_store_ticks"][i] for row in rows) / 10 for i in range(5)]
        lines.append(f"| {label} | {close:.1f} | " + " | ".join(f"{x:.1f}" for x in stores) + " |")
    release = [row["max_release_ticks"] / 10 for row in timings[2]]
    lines += ["", f"新版每次 8 MiB 运行中最慢一次窗口清理的中位数为 {median(release):.1f} µs，"
              f"12 次记录范围 {min(release):.1f}–{max(release):.1f} µs。"
              "这段内核时间包含按帧减引用及最终回池所需的填充。", ""]
    with (folder / "summary.md").open("w", encoding="utf-8", newline="\n") as output:
        output.write("\n".join(lines))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    report(parser.parse_args().folder)

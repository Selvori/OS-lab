#!/usr/bin/env python3
"""Rebuild the heap reclamation tables and figures from two schema-v2 raw files."""
import argparse
from contextlib import redirect_stdout
import hashlib
import json
from pathlib import Path
import statistics

from compare_lifecycle import label, load, table


CONFIG = ("suite", "runs", "batches", "warmup_runs", "qemu", "compiler",
          "host", "timebase_hz", "kernel_flags", "platform")
BALANCES = ("user_left", "kernel_left", "nodes_left", "slots_used", "files_used")
OPERATIONS = ("clone_slots", "destroy_slots", "heap_leaves", "leaf_checks", "leaf_reads",
              "upper_checks", "upper_reads", "heap_table_free", "table_free", "pte_visits")
MEMORY = ("user_peak", "kernel_peak", "user_alloc", "user_free", "cow_fault", "copied")
COLORS = ("#355a88", "#007969")
NAMES = ("Standard COW", "Batched unmap")


def verify_pair(paths, raw):
    assert [r["variant"] for r in raw] == ["standard-cow", "batch-unmap"]
    assert all(r["schema_version"] == 2 for r in raw)
    assert len({r["commit"] for r in raw}) == 2
    first, second = raw
    for key in CONFIG:
        assert first[key] == second[key], key
    for app in ("sh", "lifebench", "lifejob"):
        assert len({r["artifacts"][mode][app] for r in raw for mode in ("time", "counts")}) == 1, app
    left, right = [load(path) for path in paths]
    assert left.keys() == right.keys()
    for key in left:
        for field in ("heap_leaves", "heap_table_free", *MEMORY):
            assert left[key]["counts"][field] == right[key]["counts"][field], (key, field)
    return left, right


def pair(variants, key, fields):
    return ["/".join(str(int(v[key]["counts"][f])) for v in variants) for f in fields]


def summary(variants, raw, out):
    left, right = variants

    def cell(entry):
        med, p10, p90, create, reap, _, _ = entry["time"]
        return (f"{med:.3f} [{p10:.3f}, {p90:.3f}]", f"{create:.3f} / {reap:.3f}",
                f'{entry["phases"]["wait_sleep_time"]:.3f} / {entry["phases"]["exec_heap_time"]:.3f}')

    cost = []
    for k in left:
        a, b = cell(left[k]), cell(right[k])
        cost.append((label(k), a[0], b[0], a[1], b[1], a[2], b[2]))
    with (out / "summary.md").open("w", encoding="utf-8", newline="\n") as stream, redirect_stdout(stream):
        print("# 批量回收汇总\n")
        for run in raw:
            n = run["runs"] * run["batches"]
            print(f'- `{run["variant"]}` / `{run["commit"]}`，每场景 {n} 计时 + {n} 计数，失败记录 {len(run["failures"])}。')
        print("\n## 时间（ms）\n")
        table(("工作集 / 任务", "标准整轮 [P10, P90]", "批量整轮 [P10, P90]",
               "标准创建 / 销毁", "批量创建 / 销毁",
               "标准 wait 阻塞 / exec 清堆", "批量 wait 阻塞 / exec 清堆"), cost)
        print("\n## 操作数（标准/批量）\n")
        table(("工作集 / 任务", "克隆槽", "销毁槽", "撤映射/减引用", "末级判空", "末级判空读取",
               "中间表判空", "中间表判空读取", "堆剪枝页", "整轮归还表页", "整轮 PTE 探查"),
              [(label(k), *pair(variants, k, OPERATIONS)) for k in left])
        print("\n## 内存、写缺页和余额（标准/批量）\n")
        table(("工作集 / 任务", "额外用户页峰值", "额外内核页峰值", "用户页分配", "用户页回池",
               "用户 COW 写缺页", "复制 B", "剩余用户页", "剩余内核页", "剩余节点", "在用 PCB", "打开对象"),
              [(label(k), *pair(variants, k, (*MEMORY, *BALANCES))) for k in left])


def job_short(key):
    pages, writes, stride, job = key
    size = {16: "64 KiB", 256: "1 MiB", 2048: "8 MiB"}[pages]
    mode = ("exec" if job == "exec" else "read" if not writes else
            "all writes" if writes == pages else "4 spread" if stride > 1 else "4 adjacent")
    return size + "\n" + mode


def figures(variants, raw, directory):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    keys = list(variants[0])
    positions = list(range(len(keys)))
    labels = [job_short(k) for k in keys]
    n = raw[0]["runs"] * raw[0]["batches"]
    commits = "  |  ".join(f'{name} {r["commit"][:7]}' for name, r in zip(NAMES, raw))
    version = raw[0]["qemu"].split()[3]
    subtitle = f'QEMU {version} virt · 2 harts · {n} samples per scenario and build\n{commits}'
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "axes.spines.top": False,
                         "axes.spines.right": False, "axes.axisbelow": True})

    def draw(filename, title, panels, render):
        fig, axes = plt.subplots(len(panels), 1, figsize=(13.2, 7.1), sharex=True)
        fig.suptitle(title, fontsize=19, x=0.07, y=0.975, ha="left", weight="bold")
        fig.text(0.07, 0.92, subtitle, color="#3b4652", va="top")
        for ax, panel in zip(axes, panels):
            for i, (name, data) in enumerate(zip(NAMES, variants)):
                x = [pos + (i - 0.5) * 0.32 for pos in positions]
                render(ax, panel, x, data, COLORS[i], name)
            ax.set_ylabel(panel[0])
            ax.set_yscale("log")
            if panel[1] == "pte_visits":
                ax.set_yticks((10_000, 30_000, 100_000, 300_000), ("10k", "30k", "100k", "300k"))
            ax.grid(axis="y", which="major", alpha=0.25)
        axes[0].legend(loc="upper left", frameon=False, ncols=2)
        axes[-1].set_xticks(positions, labels)
        fig.subplots_adjust(left=0.075, right=0.98, top=0.81, bottom=0.13, hspace=0.19)
        fig.savefig(directory / filename, dpi=150, facecolor="white")
        plt.close(fig)

    def timing(ax, panel, x, data, color, name):
        values = [data[k]["time"][panel[1]] for k in keys]
        errors = None
        if panel[1] == 0:
            errors = [[value - data[k]["time"][1] for k, value in zip(keys, values)],
                      [data[k]["time"][2] - value for k, value in zip(keys, values)]]
        ax.errorbar(x, values, yerr=errors, fmt="o", color=color, markersize=5, capsize=3, label=name)

    def counters(ax, panel, x, data, color, name):
        ax.bar(x, [data[k]["counts"][panel[1]] for k in keys], width=0.3, color=color, label=name)

    draw("timing.png", "Fork-to-wait time and child destruction",
         (("Whole round (ms)\nMedian / P10–P90", 0), ("Wait destruction (ms)\nMedian", 4)), timing)
    draw("operations.png", "Page-table work during process reclamation",
         (("Whole round\nPTE probes", "pte_visits"), ("Heap teardown\nL0 emptiness checks", "leaf_checks")), counters)


def manifest(paths, raw, out):
    result = {"schema_version": 2, "configuration": {k: raw[0][k] for k in CONFIG},
              "runs": [{k: r[k] for k in ("variant", "commit", "artifacts", "failures")} |
                       {"raw_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                       for path, r in zip(paths, raw)]}
    result["scenario_matrix"] = [{k: c[k] for k in ("pages", "writes", "stride", "job")}
                                 for c in raw[0]["cases"] if c["mode"] == "time"]
    result["time_read_medians"] = {
        r["variant"]: statistics.median(
            t for c in r["cases"] if c["mode"] == "time"
            for batch in c["samples"] for t in batch["time_read"]) for r in raw
    }
    (out / "manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("standard", type=Path)
    parser.add_argument("batched", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tables-only", action="store_true")
    args = parser.parse_args()
    paths = (args.standard, args.batched)
    raw = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    comparison = verify_pair(paths, raw)
    args.output.mkdir(parents=True, exist_ok=True)
    summary(comparison, raw, args.output)
    manifest(paths, raw, args.output)
    if not args.tables_only:
        directory = args.output / "figures"
        directory.mkdir(exist_ok=True)
        figures(comparison, raw, directory)


if __name__ == "__main__":
    main()

"""Summarize raw guest samples; use --plot for a standalone PNG (Matplotlib)."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import random
from statistics import median

POLICIES = ("批量 COW", "共享表逐项持有", "模板持有")
LABELS = dict(a="对齐只读", q="查询与散列", u="跨表撤映射", e="立即 exec")
PRIMARY = [2048, 4, 512, "f"]


def quantile(values, q):
    data = sorted(values)
    return data[max(0, math.ceil(len(data)*q)-1)]


def per_task(row):
    return row["total"]/row["n"]/10000


def report(folder, plot):
    data = {name: json.loads((folder/(name+".json")).read_text())
            for name in ("time", "counts", "check")}
    assert all(d["commit"] == data["time"]["commit"] for d in data.values())
    for mode in data:
        for name in ("tbench", "sh", "lifejob", "tpin", "tcheck"):
            assert data[mode]["artifacts"][name] == data["time"]["artifacts"][name]
    assert data["check"]["guest_checks"] == "passed"
    binary = data["time"]["boot_pairs"]
    pairs = [[x for x in binary if x["pair"] == rep] for rep in range(12)]
    assert all(sorted(x["policy"] for x in rows) == [0,1,2] for rows in pairs)
    values = [[per_task(next(x for x in rows if x["policy"] == p))
               for rows in pairs] for p in range(3)]
    diffs = [b-a for a,b in zip(values[1],values[2])]
    rng = random.Random(20261118)
    resamples = [median(rng.choices(diffs, k=len(diffs))) for _ in range(20000)]
    bounds = [quantile(resamples, 0.025), quantile(resamples, 0.975)]
    out = ["# 三路实验结果", "",
           "主场景为 8 MiB 分散写 4 页，一个模板运行 64 个分支。每次重启 QEMU 后在同一来宾中配对三种策略，每种先预热 2 轮，12 次重启交替正反顺序。每次任务耗时包括建立和最终释放。", "",
           "| 路径 | 12 个批次的摊销中位数（ms/任务） | 中位吞吐（任务/秒） |",
           "| --- | ---: | ---: |"]
    for policy, samples in zip(POLICIES,values):
        out.append(f"| {policy} | {median(samples):.4f} | {median(1000/v for v in samples):.0f} |")
    out += ["", f"模板持有减逐项持有，每批配对差的中位数为 {median(diffs):.4f} ms/任务，"
            f"95% bootstrap 区间 [{bounds[0]:.4f}, {bounds[1]:.4f}]。"
            "以一次独立开机的配对批次为重采样单位，固定种子，重采样 20000 次。", "",
            "## 复用次数", "", "每格为整批时间/N 的中位数，单位 ms/任务。下表三组在同一来宾中执行，N=1、8 各 3 批，N=64 共 12 批。", "",
            "| N | 批量 COW | 共享表逐项持有 | 模板持有 |","| ---: | ---: | ---: | ---: |"]
    timings = data["time"]["samples"]
    reuse = []
    for n in (1,8,64):
        row = [median(per_task(x) for x in timings if x["n"]==n and x["policy"]==p)
               for p in range(3)]
        reuse.append(row)
        out.append(f"| {n} | "+" | ".join(f"{v:.4f}" for v in row)+" |")
    out += ["","## 工作负载矩阵","",
            "每个场景每种策略预热 2 轮，再测 3 批各 12 轮。每格先给出摊销中位数，再给出 36 次派生到 wait 返回的 P10 / 中位数 / P90；单位均为 ms。每轮分位数不含一次性冻结和淘汰。", "",
            "| 堆页数 / 负载 | 批量 COW | 共享表逐项持有 | 模板持有 |","| --- | ---: | ---: | ---: |"]
    groups = defaultdict(list)
    for x in timings:
        if x["n"]==12:
            groups[tuple(x["case"])].append(x)
    assert len(groups)==14
    for (pages,writes,stride,job),rows in groups.items():
        desc=LABELS.get(job, f"写 {writes} 页，步长 {stride}")
        cells=[]
        for p in range(3):
            batches=[x for x in rows if x["policy"]==p]
            rounds=[v/10000 for x in batches for v in x["rounds"]]
            assert len(rounds)==36
            cells.append(f"{median(map(per_task,batches)):.3f}（{quantile(rounds,.1):.3f} / "
                         f"{median(rounds):.3f} / {quantile(rounds,.9):.3f}）")
        out.append(f"| {pages} / {desc} | "+" | ".join(cells)+" |")
    out += ["","## 主场景的成本分解","",
            "创建、wait 销毁、首次堆写缺页为时间构建的每批相位时间/N，单位 µs。建立及关闭为每批一次的 µs。峰值是相对已预热父进程的额外页，页·ms 是计数构建中每任务额外用户与内核页的合计驻留积分；插桩会改变时长。", "",
            "| 路径 | 建立 / 关闭 | 创建 / 销毁 / 首次堆写 | 额外峰值用户 / 内核页 | 页·ms/任务 | 每任务复制堆表项 / 增加借用引用 / 退出检查表项 / 检查位图字 / 私有帧释放 |",
            "| --- | ---: | ---: | ---: | ---: | --- |"]
    counters=[]
    for p,policy in enumerate(POLICIES):
        rows=[x for x in data["counts"]["samples"] if x["n"]==64 and x["policy"]==p]
        timed=[x for x in binary if x["policy"]==p]
        def stat(key, div=1):
            return median(x[key] for x in rows)/div
        def phase(key, div=1):
            return median(x[key] for x in timed)/(10*div)
        keys=("copied_slots","retains","exit_slots","bitmap_words","private_drops")
        counts=[stat(k,64) for k in keys]
        counters.append(counts)
        assert all(x["first_writes"]==64 for x in timed)
        work=" / ".join(f"{v:g}" for v in counts) if p else "普通 COW 路径"
        out.append(f"| {policy} | {phase('setup'):.1f} / {phase('close'):.1f} | "
                   f"{phase('spawn',64):.1f} / {phase('reap',64):.1f} / {phase('first_write_ticks',64):.1f} | "
                   f"{stat('user_peak'):g} / {stat('kernel_peak'):g} | {stat('resident_page_ticks',640000):.2f} | {work} |")
    assert counters[1][0]==counters[2][0] and counters[2][4]==4
    aligned=[x for x in data["counts"]["samples"] if x["case"][-1]=="a" and x["policy"]]
    assert len(aligned)==6 and all(x["copied_slots"]==x["retains"]==x["exit_slots"]==0 for x in aligned)
    out += ["","## 长寿分支","",
            "创建者关闭模板并撤去原堆映射后，最后一个分支覆盖全部 2048 页，再持有 5 个时钟节拍。文件标记同步采样次序。相对于分配原堆之前，三条路径依次占用以下额外用户页 / 内核页。", "",
            "、".join(f"{label} {u} / {k}" for label,(u,k) in
                     zip(POLICIES,data["check"]["pin_pages"]))+"。任务结束后逐类资源余额恢复。"]
    (folder/"summary.md").write_text("\n".join(out)+"\n",encoding="utf-8")
    if plot:
        import matplotlib.pyplot as plt
        fig,(a,b)=plt.subplots(1,2,figsize=(9.6,3.9),layout="constrained")
        for p,(label,color) in enumerate(zip(("Batched COW","Shared tables + refs","Template owner"),
                                             ("#546778","#c47b27","#137e78"))):
            a.plot([0,1,2],[row[p] for row in reuse],marker="o",label=label,color=color)
        a.set(xticks=[0,1,2],xticklabels=["1","8","64"],xlabel="Jobs per snapshot",
              ylabel="End-to-end ms per job",title="8 MiB heap, four scattered writes")
        a.legend(frameon=False,fontsize=9)
        b.axhline(0,color="#778899",linewidth=1)
        b.scatter(range(1,13),diffs,color="#137e78")
        b.axhspan(*bounds,color="#137e78",alpha=.15,label="95% CI of paired median")
        b.set(xlabel="Independent QEMU boot",ylabel="Owner minus refs (ms/job)",
              title="64 jobs per snapshot")
        b.legend(frameon=False,fontsize=9)
        fig.savefig(folder/"performance.png",dpi=175)
        plt.close(fig)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder",type=Path)
    parser.add_argument("--plot",action="store_true")
    args=parser.parse_args()
    report(args.folder,args.plot)

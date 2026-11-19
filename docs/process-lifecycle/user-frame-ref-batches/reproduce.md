# 复跑方法

在 Ubuntu 的 Seele 仓库运行，QEMU 和 RISC-V 工具链版本可从原始 JSON 读取。依次完成各次 QEMU 采集。

~~~sh
out=build/heap-ref-recheck
mkdir -p "$out"
cp docs/process-lifecycle/user-frame-ref-batches/evidence/{probe_refs.py,run_mixed.py,mixed.c.txt,run_formal.py,summarize.py} "$out/"
bench() {
    python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap \
        --runs 12 --batches 3 --output "$out/$1.json"
}

git switch --detach e81ba77
bench baseline
python3 "$out/probe_refs.py" "$out" baseline

git switch --detach c791c85
bench candidate
python3 "$out/probe_refs.py" "$out" candidate

git switch --detach e81ba77
bench baseline-repeat

git switch --detach c791c85
python3 tools/test_lifecycle.py --guest-only --output "$out/guest"
python3 "$out/run_mixed.py" "$out" "$out/mixed.c.txt"
python3 "$out/run_formal.py" "$out"
python3 "$out/summarize.py" "$out" "$out"
git switch research/user-frame-ref-batches
~~~

计时及常规计数原样保存在 `raw/baseline.json`、`raw/candidate.json`、`raw/baseline-repeat.json`。每个样本按 `mode, pages, writes, stride, job` 定位。时间单位是 10 MHz 时钟刻度，每 10000 刻度为 1 ms。锁计数使用 `raw/refs-*.json` 中的独立探针样本，`retain_locks`、`release_locks`、`publish_locks` 每成功进入一次对应页池临界区加一。单页固定映像、单页 COW 缺页和局部撤映射的锁不在采样内。

`summarize.py` 核对干净提交、用户程序摘要、各批样本数、场景键和逐轮计数。在记录的两份 ELF 布局下，候选计数版用户余额有固定的一页差。混合与探针脚本仅在运行期间改动源码，按字节恢复后再启动正式构建。

# 复跑与复核

在 Ubuntu 的 `~/Work/Seele` 中运行。确认工作区干净，并串行运行各套基准。将输出写入忽略目录，保留相同的构建路径和编译配置。

~~~sh
cd ~/Work/Seele
out=build/rv64-page-copy-replay
mkdir -p "$out"
git switch --detach 6fa4fd5
python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap --runs 12 --batches 3 --output "$out/baseline.json"
git switch --detach 44802a0
python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap --runs 12 --batches 3 --output "$out/candidate.json"
git switch --detach 6fa4fd5
python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap --runs 12 --batches 3 --output "$out/baseline-repeat.json"
git switch research/rv64-page-copy
~~~

用 `artifacts[mode]` 核对跨版本的 `sh`、`lifebench`、`lifejob` SHA-256，检查三份 `failures` 为空，`dirty` 为 `false`。同一基准程序按 `time` 和 `counts` 两种模式运行。JSON 的 `variant` 是脚本选择参数，实测代码版本见 `commit`。计时表取 `mode=time` 的 `samples[*].rounds`。每批 12 个样本取中位数，36 个正式样本一起取整场景中位数和 inclusive P10／P90。除以 10 将 tick 换成 µs，再除以 1000 转成 ms。对照 `mode=counts` 时比较同场景每个样本的计数字段和资源余额。

复跑边界冒烟时，从本报告提交取得两个探针文件，使用单独构建目录执行。脚本将临时 C 代码插入启动核初始化，在 `finally` 恢复 `kernel/main.c` 并写出原始哈希。

~~~sh
cd ~/Work/Seele
doc=docs/process-lifecycle/rv64-page-copy
out=build/rv64-page-copy-smoke-replay
mkdir -p "$out"
cp "$doc"/evidence/{smoke.c.txt,run_smoke.py} "$out"
PYTHONPATH=tools OUTDIR="$out" python3 "$out/run_smoke.py"
sha256sum -c "$out/restored-main.sha256"
git diff --exit-code
make -B -j4 CONFIG=riscv64-qemu-virt-sbi build
make CONFIG=riscv64-qemu-virt-sbi run
make -B -j4 CONFIG=riscv64-visionfive2-uboot build image
dumpimage -l build/riscv64-visionfive2-uboot/kernel.itb
~~~

在 QEMU Shell 执行 `echo rv64-page-copy-pass` 查看实际回显，使用 Ctrl-A 后按 X 退出。

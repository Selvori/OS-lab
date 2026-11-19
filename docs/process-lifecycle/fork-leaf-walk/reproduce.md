# 复跑

在 Ubuntu `~/Work/Seele` 的干净工作区串行运行。基准临时磁盘和构建产物位于忽略的 `build/` 下。

~~~sh
cd ~/Work/Seele
out=build/fork-leaf-walk-replay
git switch --detach 98848fe
python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap --runs 12 --batches 3 --output "$out/baseline.json"
git switch --detach ce1fe06
python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap --runs 12 --batches 3 --output "$out/candidate.json"
python3 tools/test_lifecycle.py --guest-only --output "$out/guest"
python3 tools/bench_template.py "$out/template" check --smoke
make CONFIG=riscv64-qemu-virt-sbi build
make CONFIG=riscv64-qemu-virt-sbi run
git switch research/fork-leaf-walk
~~~

正式运行可用 Shell 的 `pwd` 确认命令可执行，用 Ctrl-A 后按 X 退出。脚本参数 `batch-unmap` 表示沿用批量回收基线协议，实测版本以 JSON 字段 `commit` 为准。

核对两份 JSON 的 `failures` 均为空、`dirty=false`，并在 `artifacts` 内核对 `time` 和 `counts` 的三个用户二进制 SHA-256。比较相同 `pages/writes/stride/job` 的场景。取 `mode=time` 中 `samples[*].rounds[*]` 的 36 个正式样本，四舍五入前将 tick 除以 10 得 µs，P10 和 P90 取 Python `statistics.quantiles(..., n=10, method="inclusive")`。按单个批次的 12 个样本另算批中位数。计数比较 `mode=counts`，每场景各自的非时间字段在全部 36 个正式样本中一致。原始 JSON 和压缩串口亦含预热轮。

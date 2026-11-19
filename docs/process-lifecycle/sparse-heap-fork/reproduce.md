# 复跑命令

在 Ubuntu 的 `~/Work/Seele` 使用干净工作区。两次测量使用现有 warm 套件的全部 11 个场景。基准脚本自动全量编译独立的 time 和 counts 内核及三个现有用户程序，构建独立磁盘镜像，并以 `make CONFIG=riscv64-qemu-virt-sbi run` 启动 QEMU。本次执行顺序如下。

~~~bash
cd ~/Work/Seele
out=build/sparse-heap-fork-2026-11-19
mkdir -p "$out"
git switch --detach 038dec28df1aecda3d0518156c1d582a627137cf
make CONFIG=riscv64-qemu-virt-sbi build
flock -n "$out/qemu.lock" python3 tools/bench_lifecycle.py --suite warm --runs 12 --batches 3 --variant batch-unmap --output "$out/baseline.json"
git switch --detach 8e59fad325ed6dd2ed8cbce50322e80be556b805
flock -n "$out/qemu.lock" python3 tools/test_lifecycle.py --guest-only --output "$out/check"
flock -n "$out/qemu.lock" python3 tools/bench_lifecycle.py --suite warm --runs 12 --batches 3 --variant batch-unmap --output "$out/optimized.json"
make CONFIG=riscv64-qemu-virt-sbi build
riscv64-linux-gnu-readelf -h build/riscv64-qemu-virt-sbi/kernel.elf
make CONFIG=riscv64-qemu-virt-sbi run
git switch research/sparse-heap-fork
~~~

实际 smoke 通过 `tools/test_lab4.py` 中的 `Machine` 调用最后一个 `make run`，等待 `seele$ `，发送 `pwd\r`，核对输出及双 hart 的 UART 就绪行，再用 Ctrl-A/X 退出。复跑时可使用新输出目录以保留原样本。两组 JSON 的 `mode=time` 下 `median.fork_time` 与 `median.elapsed` 除以 10 得 µs。用 `mode=counts` 下的 `median.clone_slots` 和 `median.user_left` 核对表格。预热样本单列在 `warmup`，正式原始样本在 `samples[].rounds`。

# 复跑采集与离线重建

## Ubuntu QEMU 采集

在干净的 `~/Work/Seele` 工作区运行。命令固定两版提交，脚本每次全量重建两个构建模式并制作独立磁盘镜像。不要在采样时同时运行其他 QEMU 或本项目回归。使用一个尚未使用的 `build/batch-replay` 路径保留新样本。切换提交前请收好本地改动。

```sh
cd ~/Work/Seele
git switch --detach db3d44faab559820de3b4023e1cd8121a16f9a9f
python3 tools/bench_lifecycle.py --suite warm --runs 12 --batches 3 --variant standard-cow --output build/batch-replay/standard.json
git switch --detach 15dd3f160c6fa2f240ae4b639c9fb7146806dc48
python3 tools/bench_lifecycle.py --suite warm --runs 12 --batches 3 --variant batch-unmap --output build/batch-replay/batched.json
git switch research/process-lifecycle
python3 tools/plot_reclaim.py build/batch-replay/standard.json build/batch-replay/batched.json --output build/batch-replay/report --tables-only
```

全量回归通过 `python3 tools/test_lifecycle.py --output build/batch-replay/regression` 执行。检测用的脚本会还原临时源码并重建正式 ELF 与 VisionFive 2 FIT。时间值随 QEMU 和宿主调度变化，操作计数用于独立核对。正式运行日志在输出 JSON 旁，归档串口日志统一换行并移除末尾空白。实际执行参数、构建产物摘要及两版对照条件见[清单](manifest.json)。

## 从归档数据生成报告

从仓库或 Windows 归档根目录用 Python 3.10+ 运行下列命令。绘图需要 Matplotlib，本轮使用 3.10.7。仅重建汇总表和清单时加 `--tables-only`，只用 Python 标准库。两种方式均无需 QEMU 和 RISC-V 工具链。PowerShell 可将多行命令合为一行。

```sh
python3 tools/plot_reclaim.py docs/process-lifecycle/batch-reclaim/raw/standard.json docs/process-lifecycle/batch-reclaim/raw/batched.json --output build/batch-offline
```

输出为 `summary.md`、`manifest.json`、`figures/timing.png` 和 `figures/operations.png`。可用输出目录独立比较重建结果，归档原件保持不变。

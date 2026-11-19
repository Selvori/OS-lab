# 复跑

在 Ubuntu 的 ~/Work/Seele 中使用 QEMU 5.1.0、riscv64-linux-gnu-gcc 11.4 和 Python 3。报告提交包含全部证据。在报告提交先把验证脚本复制到忽略目录，再依次切换至两个测量版本。

~~~sh
report=docs/process-lifecycle/sequential-cow-prefetch
out=build/cow-prefetch-recheck
mkdir -p "$out/evidence" "$out/raw"
cp "$report"/evidence/*.py "$report"/evidence/smoke.c "$out/evidence/"
git switch --detach e7fba499f66d07a601dc24007718edb226b03f08
python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap --runs 12 --batches 3 --output "$out/raw/baseline.json"
git switch --detach c614c2a8244b4ac5884d085951f216d4348c0ce6
python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap --runs 12 --batches 3 --output "$out/raw/candidate.json"
git switch --detach e7fba499f66d07a601dc24007718edb226b03f08
python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap --runs 12 --batches 3 --output "$out/raw/baseline-repeat.json"
git switch research/sequential-cow-prefetch
python3 "$out/evidence/summarize.py" "$out"
python3 "$out/evidence/run_smoke.py" "$out"
python3 tools/test_lifecycle.py --guest-only --output "$out/lifecycle"
python3 tools/bench_template.py "$out/template" check --smoke
python3 "$out/evidence/run_formal.py" "$out"
~~~

每个基准命令自身依次运行 time 和 counts。保持串行执行以减少互相干扰。汇总脚本会检查所有样本、分组平衡及用户程序摘要。冒烟和两项已有回归采用各自的探针构建和独立镜像，无需临时编辑内核。正式检查生成 VisionFive 2 FIT，并验证 QEMU Shell 和 ELF 入口。

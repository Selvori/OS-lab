# Seele · 进程创建与回收

本分支在 Lab 9 基础上实现标准写时复制 fork 和按 Sv39 末级表批量回收，并保留可检出的深拷贝与标准 COW 基准。父子独立建表、共享用户帧。写缺页和内核代写按需复制，wait 在父栈回收。磁盘 Shell 与程序 exec 保留 Lab 9 用法。

~~~sh
make CONFIG=riscv64-qemu-virt-sbi run
python3 tools/test_lifecycle.py
python3 tools/bench_lifecycle.py --suite warm --variant batch-unmap --output build/process-lifecycle/warm.json
make CONFIG=riscv64-visionfive2-uboot build image
~~~

[批量回收报告与复跑](docs/process-lifecycle/batch-reclaim/README.md) · [深拷贝与标准 COW 比较](docs/process-lifecycle/comparison.md) · [研究过程及验收](docs/process-lifecycle/README.md) · [资源所有权](docs/process-lifecycle/ownership.md) · [Lab 9 功能与板端用法](docs/lab-9-baseline.md)

[Sv39 模板所有权协议、可执行模型和基准](docs/process-lifecycle/template-ownership/README.md)

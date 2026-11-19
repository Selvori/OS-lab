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

[候选模板内核原型及三路对照](docs/process-lifecycle/template-candidate/README.md) 已在独立研究分支验证。普通 fork 保留批量 COW；模板操作通过显式系统调用使用。

[关闭模板后的窗口提前回收](docs/process-lifecycle/template-window-reclaim/README.md) 提供独立策略 `freeze(2)`。它在最后一个分支放弃某个 2 MiB 窗口里的全部借用后归还模板的原始堆帧引用。QEMU 的 8 MiB 长寿分支少占 2048 个用户页，大窗口末次写增加同步释放开销。

[普通 fork 稀疏堆克隆](docs/process-lifecycle/sparse-heap-fork/README.md) 按空闲区间跳过未映射堆槽。64 KiB 只读负载在 QEMU 上的 fork 到 wait 中位数由 137.20 µs 降到 99.55 µs，堆槽探查由 4096 次降到 16 次。

# Seele · 进程创建与回收

本分支在 Lab 9 的磁盘 Shell 和 exec 基础上实现了写时复制 fork、按 Sv39 末级表批量回收、模板派生和有界回收。普通 fork 为父子进程分别建表并共享用户帧。写缺页和内核代写按需复制，wait 在父栈回收。

QEMU 默认由启动 hart 运行用户进程。`DUAL_HART_USERS=1` 允许另一颗 hart 执行一个符合条件的用户上下文，涵盖普通进程和 `freeze(1)`、`freeze(9)` 派生的分支。调度、系统调用、COW、页表修改和回收由启动 hart 处理。工作 hart 切回内核页表并刷新本地翻译后交回现场。VisionFive 2 由启动 hart 运行用户进程。详见[当前架构规格](design.yaml)、[进程与调度](modules/process.yaml)、[用户陷阱](modules/user-trap.yaml)和[系统调用](modules/syscall.yaml)。

~~~sh
make CONFIG=riscv64-qemu-virt-sbi run
make CONFIG=riscv64-qemu-virt-sbi DUAL_HART_USERS=1 run
make CONFIG=riscv64-visionfive2-uboot build image
~~~

QEMU 双核配置默认使用独立构建目录和磁盘镜像。以下回归从完整 Git checkout 运行，脚本用 Git 跟踪列表核对测试前后的源文件哈希。

~~~sh
python3 tools/test_lifecycle.py
~~~

深拷贝、标准 COW 和各项优化的对照版本及测量命令见下方报告。

[批量回收报告与复跑](docs/process-lifecycle/batch-reclaim/README.md) · [深拷贝与标准 COW 比较](docs/process-lifecycle/comparison.md) · [研究过程及验收](docs/process-lifecycle/README.md) · [标准 COW 基线的资源所有权](docs/process-lifecycle/ownership.md) · [Lab 9 功能与板端用法](docs/lab-9-baseline.md)

[Sv39 模板所有权协议、可执行模型和基准](docs/process-lifecycle/template-ownership/README.md)

[候选模板内核原型及三路对照](docs/process-lifecycle/template-candidate/README.md) 已在独立研究分支验证。普通 fork 保留批量 COW；模板操作通过显式系统调用使用。

[关闭模板后的窗口提前回收](docs/process-lifecycle/template-window-reclaim/README.md) 提供独立策略 `freeze(2)`。它在最后一个分支放弃某个 2 MiB 窗口里的全部借用后归还模板的原始堆帧引用。QEMU 的 8 MiB 长寿分支少占 2048 个用户页，大窗口末次写增加同步释放开销。

[普通 fork 稀疏堆克隆](docs/process-lifecycle/sparse-heap-fork/README.md) 按空闲区间跳过未映射堆槽。64 KiB 只读负载在 QEMU 上的 fork 到 wait 中位数由 137.20 µs 降到 99.55 µs，堆槽探查由 4096 次降到 16 次。

[RV64 对齐整页填充与复制](docs/process-lifecycle/rv64-page-copy/README.md) 在内核清零、回池填充和 COW 复制使用 64 位访问。QEMU 的 8 MiB 全写负载从 fork 到 wait 的中位数由 45.850 ms 降至 29.098 ms，复测原版为 46.287 ms。

[普通 fork 复用 Sv39 末级页表](docs/process-lifecycle/fork-leaf-walk/README.md)将已映射堆段按 2 MiB 边界切分，复用父、子末级表。QEMU 的 8 MiB 只读整轮中位数从 1.153 ms 降至 0.904 ms，fork 相位从 0.550 ms 降至 0.322 ms。

[普通 COW 堆帧引用分批更新](docs/process-lifecycle/user-frame-ref-batches/README.md) 在私有堆克隆和整体清理路径中每批更新至多 32 个引用。QEMU 的 8 MiB 只读普通 fork 至 wait 中位数由 0.905 降至 0.443 ms。

[普通私有堆 COW 顺序写提前分离](docs/process-lifecycle/sequential-cow-prefetch/README.md) 在同一 Sv39 末级表命中相邻堆页写缺页后，最多提前复制四页。QEMU 的 8 MiB 全写整轮中位数从 30.181 ms 降至 15.675 ms，复测基线为 28.896 ms。集中写四页会额外复制两页，方案保留在独立研究分支。

[模板关闭后的有界逐页回收](docs/process-lifecycle/template-bounded-reclaim/README.md)通过显式 `freeze(6)` 选择。8 MiB 五窗十页钉住负载在分支存续期间归还 2038 帧，单调度机会全队列限额为 32 次确认、8 次减引用、1 次固定析构。开放期含最终排空的摊销未通过 5% 门槛。

[有界逐页回收的独立验收](docs/process-lifecycle/template-bounded-reclaim/independent/README.md) 逐帧核对两条活分支的映射及物理页池，并从 12 次独立开机原始样本复算性能门槛。

[七种模板策略的同负载数据和优劣](docs/process-lifecycle/template-bounded-reclaim/strategy-comparison/README.md)给出 14 次独立开机的每任务耗时、计数与五窗保留试验。

显式 `freeze(8)` 在模板关闭时无活分支且关闭者主动让出的条件下，通过调度器空闲轮排空。QEMU 主样本的排空中位数较策略 6 从 7.666 ms 降至 1.716 ms，五窗活分支仍提前回池 2038 页。[空闲排空结果及复跑](docs/process-lifecycle/template-idle-drain/README.md)

[零分支关闭后的末次引用释放](docs/process-lifecycle/template-terminal-release/README.md) 以显式 `freeze(9)` 缩短实际排空，并保留活分支逐页回收。报告包含 5% 门槛、双分支五窗和带洞物理帧检查。

[普通进程的双 hart 用户态研究](docs/process-lifecycle/dual-hart-users/syscall-affinity/README.md) 中，相同的双进程纯计算负载在四组配对开机中的完成时间中位数由 438.00 ms 降至 218.69 ms。GDB 同时观察到两个 hart 在不同用户页表下执行 U 态。启动 hart 在用户态定时器采样时观察系统调用次数，密集调用的任务会暂留本地。

[模板分支双 hart 执行与回收见证](docs/process-lifecycle/dual-hart-template-branches/README.md) 在同一 QEMU 双 hart 开关下开放策略 1 和 9 的用户态派发。关闭后两条真实兄弟分支仍可并行，8 MiB 双分支计算配对中位加速为 1.97× 和 1.96×。回收器在策略 9 的双 U 态五窗见证中归还 2038 个原帧。

关闭句柄后仍有活分支的 `freeze(9)`，在最后一个借用者解除映射后可以直接释放剩余原帧。8 MiB 双分支短任务的末次 wait 后排空从 22.04 ms 降至 3.70 ms，已退出子进程的最后一次 wait 从 114.85 µs 到 117.50 µs。[配对基准、四种组合与物理页证据](docs/process-lifecycle/template-last-detach-drain/README.md)

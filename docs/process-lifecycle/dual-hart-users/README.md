# 普通进程的双 hart 用户态执行

`research/dual-hart-users` 从 `research/template-terminal-release` 的 `98793dbb273fa27e959b23f80107ede66abb181e` 建立。签署的实现和复跑工具提交为 `2ab40ba3015751b852a835f7a5ae85dcf5a58fea`。默认构建沿用单 hart 调度，QEMU virt 可用 `DUAL_HART_USERS=1` 打开原型。

## 执行与所有权

- 启动 hart 持有全部 PCB、可运行进程选择、系统调用、COW 缺页处理、退出、父子唤醒及模板管理。只有存在一个留在启动 hart 的可运行任务时，调度器才给另一 hart 提交一个普通用户上下文。直接从普通 fork 创建的用户上下文及被定时器或 IPI 从 U 态抢占的上下文可以交接。挂起的内核调用继续在原进程内核栈恢复。init、模板分支、持模板句柄或待主动让出做回收的进程留在启动 hart。
- 交接只借用独占的 trapframe 和用户根页表。PCB 的 `PROC_REMOTE` 状态阻止启动 hart 运行或销毁该进程。单槽邮箱通过 release/acquire 传递任务及结果，两方向用 SBI IPI 唤醒。启动 hart 在 U 态收到回报时让出到调度器，空闲时用待处理的 IPI 唤醒 WFI。用户定时器各在本 hart 服务，只有启动 hart 推进全局睡眠 tick。接收方向按实际启动 hart 推导。
- 第二 hart 在 acquire 后执行本地 `fence.i`，再经既有 trampoline 加载用户 satp 并返回 U 态。内核在启动 hart 装入用户代码后执行现有的本地同步，邮箱发布使写入对接收 hart 可见。第二 hart 的用户陷阱切回内核根并完成本地 `sfence.vma`，使用独立常驻陷阱栈，回到常驻调度栈后才发布完成。故障地址、原因和保存的用户现场交回。时钟抢占后可重新派发，系统调用及同步异常在启动 hart 的进程栈处理。外部设备中断留在接收 hart 服务。
- 邮箱交回之后，另一 hart 已无该用户根和每进程内核栈的驻留翻译，启动 hart 可依原规则执行 COW、exec、wait 回收及槽复用。普通 fork 的共享可写页受 COW 约束，帧由各自引用保留。模板根始终未装入 satp，模板分支的借用翻译仍限于启动 hart。启动 hart 只在第二 hart 无挂起任务时将空闲回收批计为空闲。
- 正式无探针 QEMU 构建增加 1672 字节 text 和 4624 字节 BSS。其中 4096 字节用于独立陷阱栈。交接成本包括正常的 trampoline 翻译失效、接收侧每次派发的指令缓存同步、两次邮箱发布和 SBI 通知、以及一次有界 PCB 搜索。频繁系统调用或缺页的负载会受启动 hart 串行服务限制。本轮未单测每次交接的时延，也未测混合负载、多于两个 hart 或开发板上的动态执行。

## 同负载配对计时

Ubuntu 22.04.5 LTS，8 vCPU，QEMU 5.1.0、OpenSBI 0.7（SBI 0.2）、RISC-V GCC 11.4.0。两模式均使用 `virt`、128 MiB、`-smp 2 -accel tcg,thread=multi`。两个内核都用 `LIFECYCLE=time` 开放 U 态 `time` 计数器，并使用同一份 Shell、`dualbench` 和磁盘镜像。单 hart 模式让第二 hart 继续处理设备中断，只有启动 hart 执行用户指令。

每个独立开机先预热一轮，再计时一轮。每轮父子各做 128,000,000 次同一无系统调用计算循环，测量从 fork 前到 wait 返回的 10 MHz 来宾时间。每个样本核对两次计算、COW 栈内容、wait PID 及退出状态。两方式均得到父子校验和 `1209409537`，主机复算一致。GDB 在另开的见证进程内取样。

| 配对 | 开机顺序 | 单 hart（ms） | 双 hart（ms） | 时间减少 |
| --- | --- | ---: | ---: | ---: |
| 1 | 单 → 双 | 328.046 | 174.676 | 46.75% |
| 2 | 双 → 单 | 334.597 | 171.622 | 48.71% |
| 3 | 单 → 双 | 331.500 | 172.622 | 47.93% |
| 4 | 双 → 单 | 327.896 | 178.235 | 45.64% |
| 中位数 | | 329.773 | 173.649 | 47.34%（配对） |

配对加速中位数为 **1.90×**。这是四个配对下该计算负载的结果，覆盖固定工作量及 fork/wait 完成时间。[原始样本](paired.csv) · [镜像摘要](artifacts.txt) · [完整配对串口](paired-serial.txt)

## 同时驻留 U 态

一次 GDB all-stop 快照中，hart 0 和 hart 1 同时报告 `priv=0`，PC 分别为 `0x1136`、`0x1142`，都在用户二进制的 `calculate` 符号区间 `[0x1122, 0x114c)` 内。两者的 `satp` 分别是 `0x8000000000080337`、`0x8000000000080330`。QEMU 同一快照报告两个不同的宿主 vCPU 线程 ID `93903` 和 `93904`。单 hart 对照中另一 hart 在 S 态。正式时间组的双 hart 模式由 hart 1 启动，正式寄存器见证由 hart 0 启动，两种身份均实际运行。预试验还保留了启动 hart 为 1 时的双 U 态见证。[见证 CSV](witness.csv) · [双 hart 原始寄存器](witness-mode-2-gdb.txt) · [单 hart 原始寄存器](witness-mode-1-gdb.txt) · [启动 hart 为 1 的预试验寄存器](witness-owner-1-gdb.txt)

## 回归与复跑

- 开启原型的检查内核通过八轮既有生命周期测试，包括 COW 隔离、失败回滚、异常退出和资源余额。正式 ELF 在开关两态都通过标准 `make run` 的外部 `echo`，入口都是 `0x80200000`。VisionFive 2 默认路径通过 ELF 和 FIT 构建，入口 `0x40200000`。
- `bcheck 9` 实测两个活分支在五窗保留 10 帧时回池 2038 帧，随后全部 2048 帧回池。原检查假定子进程按指定 PID 顺序退出。预试验捕获合法的倒序退出后，检查改为接受任一顺序并逐一核对 PID 和状态 73。原型的 `bclose 9 idle` 也通过。[生命周期与模板串口](smoke-serial.log.gz)
- 用同一检查构建另外运行既有 `byield ref9` 和 `byield sparse9`。零活分支关闭时，创建者仍持有的 2048 帧均保留，稀疏撤映射时 2022 帧全部回池。两项均无逐页确认，符合原策略 9 的末次引用路径。[终态专项串口](terminal-serial.log)

```sh
make CONFIG=riscv64-qemu-virt-sbi run
make CONFIG=riscv64-qemu-virt-sbi DUAL_HART_USERS=1 run
python3 tools/bench_dual_hart.py build/dual-hart/replay --pairs 4 --iterations 128000000
python3 tools/bench_dual_hart.py build/dual-hart/replay-witness --witness-only
python3 tools/bench_dual_hart.py build/dual-hart/replay-smoke --smoke
make CONFIG=riscv64-visionfive2-uboot build image
```

开启原型时，默认构建目录带 `-dual-hart` 和已选的生命周期探针模式。自定义 `BUILD` 时为每种配置使用独立目录。见证工具依赖 `gdb-multiarch`。

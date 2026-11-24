# 两个普通子进程从系统调用转入计算

两个普通 fork 子进程各连续执行 4096 次 getpid 并逐次核对 PID，随后进入各自 8500 万次的整数运算。每个子进程在计算前写出一个串口标记，父进程通过 wait 验证两个不同 PID 的退出值分别为 895398383、45421316，并确认已经收完所有子进程。

在自适应实现 a21fc25 上，两条标记到达后暂停整个 QEMU。第一次采样读到 hart 0 和 hart 1 同时处于 U 态，PC 均为 0x10e4，位于用户程序 crunch 循环。satp 分别为 0x800000000008033f 和 0x8000000000080338，对应两个不同地址空间。这次由 hart 0 启动。[寄存器与镜像摘要](after-syscalls.json) · [串口输出](after-syscalls-serial.txt)

同一个内核可以在密集系统调用阶段结束后再次并行执行这两个进程。寄存器快照用于确认并行执行，计时来自不接调试器的独立开机。[四组配对耗时](../README.md)

作为对照，另两次取样使用没有系统调用准备阶段的整数程序。旧内核由 hart 1 启动，hart 0 在四次全停快照中均位于 arch_park 的 S 态。初版 2ab40ba 已能观测到两个不同用户根同时执行整数循环。[旧内核寄存器](boot-only.json) · [初版双核寄存器](first-parallel-version.json)

调试器连接及暂停会影响时序。一次短计算复跑在采样时只读到一颗核处于循环；两次退出值均正确。复跑脚本把每个子进程的计算阶段延长为八段，每段 8500 万次并逐段核对同一结果，以便观察。这个版本也读到了同处 0x10e4 的两个不同用户页表。[延长窗口后的原始 GDB 输出](extended-gdb.txt)

在 Ubuntu 仓库根目录执行以下命令。最后一个参数指定本次开机的输出目录。

~~~sh
python3 docs/process-lifecycle/dual-hart-users/syscall-affinity/phase-transition/check_phase.py build/dual-hart/phase-replay
~~~

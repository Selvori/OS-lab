# Lab 3 实验报告：中断异常初步

- 项目：Seele Kernel
- 实验分支：`lab-3`
- 起始提交：Lab 2 的 `c021a1065f32db22deaad92c847bddbcce564799`
- 报告与简要验收日期：2026-10-10
- 状态：QEMU 核心功能及简要验收通过；报告已整理，代码尚未提交、推送。

## 一、实验目的与范围

在 Lab 2 已有的物理页分配器、Sv39 共享内核页表和双核启动机制上，建立能够保存现场、分发中断并返回原执行流的内核陷阱框架，完成 UART 接收中断与 SBI 时钟中断。

本实验采用课件第 11 页的 OpenSBI 路径：内核运行于 S-mode，底层定时器由固件管理。同步异常仅提供诊断并停止故障 hart，不实现异常恢复。用户态陷阱、进程调度、软件 IPI、嵌套中断及浮点/向量现场保存均不在本次范围内。

## 二、实验环境

| 项目 | 配置 |
| --- | --- |
| 开发位置 | Ubuntu 22.04.5 LTS，`~/Work/Seele` |
| 编译工具链 | `riscv64-linux-gnu-*`，启用 `-Wall -Wextra -Werror` |
| 模拟器 | QEMU 5.1.0，virt 平台，两个 hart，128 MiB DRAM |
| 固件 | OpenSBI v0.7，运行时 SBI 0.2 |
| 内核模式 | S-mode，Sv39，设备与内核采用恒等映射 |
| QEMU ELF 入口 | `0x80200000` |
| 辅助平台 | VisionFive2，仅验证 ELF 构建，入口 `0x40200000` |

代码在个人仓库中独立实现；xv6 与 QEMU 源码仅作只读参考，没有直接修改或复制整个参考项目。

## 三、实现结构与关键文件

| 文件 | 作用与学习重点 |
| --- | --- |
| `arch/riscv64/kernel_vector.S` | 保存、恢复全部整数寄存器与返回所需 CSR；调用 C 分发器并执行 sret |
| `arch/riscv64/include/asm/trap.h` | 汇编/C 共用的陷阱帧布局，编译期检查偏移和大小 |
| `arch/riscv64/trap.c` | 每核 stvec 初始化、原因识别、外部/时钟中断分发、故障诊断 |
| `include/kernel/trap.h` | 暴露陷阱初始化与处理接口 |
| `drivers/irq/plic.c`、`plic.h` | 全局优先级、每核使能与阈值、claim/complete |
| `arch/riscv64/include/asm/io.h` | 按宽度访问设备，使用 volatile 与 fence 保证 MMIO 访问和顺序 |
| `drivers/serial/uart16550.c` | UART 接收中断使能、非阻塞取字节、排空 FIFO |
| `kernel/lib/console.c` | 输入回显和换行转换，复用已有打印锁 |
| `arch/riscv64/timer.c`、`include/kernel/timer.h` | 每核时钟预约、重新预约、计数与查询 |
| `arch/riscv64/boot/sbi.c` | SBI TIME 调用封装，向固件传入绝对到期时间 |
| `kernel/main.c` | 区分全局初始化与每核初始化，最后开启中断 |

构建列表和平台配置同步加入新文件、UART IRQ、PLIC context、访问宽度与 timebase 等参数。Lab 1 的自旋锁和 Lab 2 的页表继续提供基础设施。

## 四、关键原理

### 4.1 陷阱入口、现场保存与返回

硬件陷入时会写入 sepc、scause、stval 并更新 sstatus，但不会替内核保存全部通用寄存器。入口在当前内核栈上分配 288 字节，保持 16 字节对齐：

| 字节偏移 | 内容 |
| --- | --- |
| 0–248 | x[0]–x[31]，每项 8 字节；x0 槽为零，x2 保存陷入前 sp |
| 256 | sstatus |
| 264 | sepc |
| 272 | scause |
| 280 | stval |

中断可以发生在任意指令边界，因此不能仅按普通函数调用约定保存 callee-saved 寄存器。入口先保护 t0，随后保存原 sp、gp、tp 和其余整数寄存器，再将现场指针传给 C 函数。

返回时恢复 sstatus 与 sepc，恢复阶段继续保持 SIE=0，最后恢复寄存器、sp 并执行 sret。SPIE 保存陷入前的 SIE，sret 据此恢复中断状态。普通中断保持原 sepc，不统一增加 4；这与需要跳过特定陷阱指令的处理情形不同。

scause 最高位为 1 表示中断，低位 1、5、9 分别表示 S-mode 软件、时钟、外部中断。当前软件中断未启用；未知原因或同步异常进入不依赖打印锁的紧急诊断路径，输出 hart、原因、地址和状态后停止故障 hart。

### 4.2 UART 与 PLIC

```text
输入 → UART FIFO → PLIC → S-mode 外部中断
     → kernel_vector → trap_kernel_handler
     → plic_claim → uart_intr → uart_getc → console_intr/printf
     → plic_complete → 恢复现场 → sret
```

启动核将 UART 的 PLIC 优先级设为 1；每核只启用自己的 S-context 中的 UART 源，阈值设为 0。claim 返回设备编号，UART FIFO 排空后才 complete。同一请求竞争下另一核可能得到 claim=0，这是合法情况，直接返回且不 complete(0)。

CPU 的外部中断原因 9 与 QEMU 的 UART 设备 IRQ 10 不是同一层编号。PLIC 不携带字符内容，字符须从 UART RHR 读取。

uart_getc 无数据返回 -1，有数据返回 0–255，因此 NUL 和高位字节仍是有效输入。uart_intr 循环读取当前可用字节；发送仍轮询，接收回显使用打印锁。独立 CR/LF 均输出 CRLF，输入 CRLF 会产生两次换行；没有行编辑、软件输入队列或错误字节统计。

### 4.3 SBI 时钟中断

```text
每核读取 time → SBI TIME 预约第一次到期
到期 → 固件投递 S-mode 时钟中断（原因 5）
     → kernel_vector → trap_kernel_handler → timer_interrupt_handler
     → 重新预约 → 本核 ticks 加一 → sret
```

SBI TIME 的扩展编号为 0x54494d45，功能编号为 0。RV64 在 a0 中传入完整 64 位绝对时间，返回错误时进入 panic。时钟不经 PLIC，不使用 claim/complete，也不要求内核直接映射 CLINT。

```c
TIMER_INTERVAL = TIMEBASE_HZ / TIMER_HZ;
sbi_set_timer(csr_read(time) + TIMER_INTERVAL);
```

当前 TIMER_HZ=100。QEMU timebase 为 10 MHz，间隔为 100000；VisionFive2 为 4 MHz，间隔为 40000。timebase 不等于 CPU 主频或 UART 时钟。

周期性来自每次中断后的重新预约。按当前时间设置下一次事件可避免长时间关中断后集中补发旧节拍，但会随处理延迟产生漂移。timer_ticks(cpu) 是该核实际处理次数，不是精确墙上时间；不能将多核计数相加当系统运行时间。计数采用 relaxed 原子操作支持跨核查询，不承担其他共享对象的发布职责。

### 4.4 多核初始化与锁

```text
启动核：打印/页分配/共享页表初始化 → 激活页表 → 安装入口
       → 全局 PLIC 与 UART 初始化 → 启动从核 → release-store started
从核：acquire-load started → 激活页表 → 安装入口
每核：PLIC context → 首次时钟预约 → sie.SEIE/STIE → 开 SIE → wfi
```

started 发布共享初始化结果，不要求所有 CPU 同时完成本地初始化。每核在准备完成后独立开启中断；启动 hart 通过实际身份判断，不写死 CPU 0。

自旋锁的原子 acquire/release 保证跨核互斥与可见性；push_off/pop_off 防止本核被申请同一把锁的中断处理器打断。必须先解锁再恢复中断。中断中 printf 保存的原始状态为 SIE=0，释放锁不会提前开中断，最终由 sret 恢复。

## 五、实施过程与问题处理

1. 从已确认完成的 Lab 2 提交建立 lab-3，保留 OpenSBI 启动和共享页表。
2. 建立陷阱帧、汇编入口与 C 分发，通过临时软件中断验证返回现场。
3. 接入 PLIC 和 UART 接收，处理双核竞争的空 claim，并统一 MMIO 顺序。
4. 增加 SBI TIME 预约、每核计数和 STIE，验证屏蔽期间 pending 与恢复行为。
5. 综合验证启动时序、双核打印、页分配、锁和中断，恢复全部临时代码。
6. 2026-10-10 对正式版本进行简要复验，整理本报告与文本证据。

关键决策包括：按实际 OpenSBI 路径实现时钟，普通中断不修改 sepc；按不同平台配置 PLIC context 与 UART 访问宽度；未知陷阱使用无打印锁的输出避免故障诊断死锁。

测试取证中，QEMU 5.1.0 的 -d int 输出未提供脚本预期的记录，随后改用临时计数和 GDB。首次脚本插入因缩进不匹配未生效，核对后修正并重跑；报告引用的是修正后数据。临时代码恢复通过逐字节比较和哈希核对，再重新构建运行。

## 六、本次简要验收（2026-10-10）

本次使用正式内核，没有重新插入压力测试代码。执行命令：

```bash
cd ~/Work/Seele
make CONFIG=riscv64-qemu-virt-sbi build
riscv64-linux-gnu-readelf -h build/riscv64-qemu-virt-sbi/kernel.elf
python3 -u build/lab3-step4/test_timer_uart.py final
make CONFIG=riscv64-qemu-virt-sbi run
make CONFIG=riscv64-visionfive2-uboot build
git diff --check
```

测试脚本位于本实验工作副本的忽略目录；新克隆仓库不会包含该脚本。基本构建、运行和人工输入回显不依赖它。退出 QEMU 使用 Ctrl-A 后按 X。

| 验收项 | 本次实际结果 |
| --- | --- |
| QEMU/VisionFive2 构建 | 均通过，入口分别 0x80200000 / 0x40200000 |
| 双核启动 | 两核使用相同 Sv39 根页表，时钟与 UART 就绪 |
| UART 回归 | 5777 输入字节逐字节比对通过 |
| 输入覆盖 | 单字节、64 批、4096 字节长串、NUL/DEL/0x80/0xff、CR/LF、空闲后再次输入 |
| 输入期间时钟 | 两核计数 [36,36] → [336,336] |
| 空闲期间时钟 | 后续快照 [440,440]、[543,543]、[647,647] |
| 实际 make run | 回显 Lab3 acceptance PASS，无 PANIC，正常退出 |
| 整洁性 | git diff --check 通过，ELF 未发现 probe_/test_ext/test_rx 测试符号 |

时钟快照包含 GDB 连接及暂停开销，用于确认持续推进，不作为精密频率测量。`Lab3 acceptance PASS` 是主机输入后的串口回显，独立的测试断言才用于判断验收通过。

本次原始证据：[启动与回显](lab-3-evidence/2026-10-10-make-run.txt)、[UART 与时钟回归](lab-3-evidence/2026-10-10-uart-timer.txt)。这些文件是真实文本输出，不是运行截图。

## 七、此前阶段的验证汇总

以下结果来自此前已执行的阶段测试，本次仅整理归档，没有重复运行所有负向与压力实验。

| 日期/阶段 | 测试 | 结果 |
| --- | --- | --- |
| 09-29 陷阱 | 两核各 1024 次临时软件中断，刻意破坏实时 CSR 与 caller-saved 寄存器 | 全部整数现场、sepc、SIE 恢复通过 |
| 09-29 故障路径 | 持打印锁执行非法指令；触发未安装软件中断 | 正确输出诊断并 panic，没有打印锁死锁 |
| 09-29 UART | 双核、仅 CPU0、仅 CPU1 路由 | 每轮 5777 字节均通过；单核分别为 5777/0、0/5777 |
| 09-29 PLIC 竞争 | 双核 claim=0 | 样本 255/300 次，正常返回，空闲计数不增长 |
| 10-09 时钟 | 两层 push_off，屏蔽 250 ms | 计数不变、STIP pending；恢复后两核继续处理 20 次 |
| 10-09 启动 | 12 次冷启动 | 全部通过，启动 hart 覆盖 0 和 1 |
| 10-09 时序 | 分别延迟 CPU0/CPU1 初始化一秒 | 另一核仍可处理时钟与输入，延迟核随后正常运行 |
| 10-09 综合负载 | 双核锁、页分配、打印，同时输入 UART | 计数 256；512 对分配/释放；32 条完整打印；25600 字节回显数量匹配 |

综合负载为重复 @ 字节的数量与打印完整性检查，不等同于任意字节序列的顺序验证；任意测试载荷由 5777 字节回归进行逐字节比较。[综合测试摘要](lab-3-evidence/2026-10-09-multicore.json)

## 八、平台差异与未验证项

| 参数 | QEMU virt | VisionFive2/JH7110 |
| --- | --- | --- |
| UART IRQ | 10 | 32 |
| UART 访问宽度/间隔 | 8 位 / 1 字节 | 32 位 / 4 字节 |
| S-context | 2×hart+1 | 2×hart |
| timebase | 10 MHz | 4 MHz |
| 本报告验证范围 | 双核实际运行 | ELF 构建 |

当前只支持 S-mode 整数上下文，沿用内核栈和页表；故障 panic 只停止当前 hart。没有用户态、调度、嵌套中断、输入队列与异常恢复。UART 同步回显可能增加中断延迟，测试未保证持续满速真实串口无丢字、锁公平性或严格实时性。

VisionFive2 开发板和 FIT 镜像未验证；历史 FIT 构建曾因缺少 dtc 失败，本次未安装依赖或重试打包。Lab 3 截图尚未采集，后续实际截图应保存到 pictures/。代码与报告尚未提交、推送，不宣称仓库归档已全部完成。

## 九、实验结论

本实验完成了从硬件事件到内核处理再返回原执行流的完整中断路径。UART 验证了共享外设的领取、服务与完成，SBI 时钟验证了每核定时事件和重新预约，多核测试验证了共享初始化、局部中断状态与自旋锁的配合。

QEMU 范围内的核心实验目标与本次简要验收已通过。后续进程实验可以复用陷阱入口、原因分发和时钟接口，但仍需补充用户现场、地址空间切换及调度语义。

## 参考材料

- 课程 lab-3.pptx，第 6、8、11 页；本地项目规范与实验记录。
- QEMU 5.1.0 的 virt、sifive_plic、sifive_clint、serial 源码（只读核对）。
- [RISC-V PLIC 规范](https://github.com/riscv/riscv-plic-spec/blob/master/riscv-plic.adoc)。
- [SBI TIME 规范](https://raw.githubusercontent.com/riscv-non-isa/riscv-sbi-doc/master/src/ext-time.adoc)。
- Linux 的 [jh7110.dtsi](https://raw.githubusercontent.com/torvalds/linux/master/arch/riscv/boot/dts/starfive/jh7110.dtsi) 与 [jh7110-common.dtsi](https://raw.githubusercontent.com/torvalds/linux/master/arch/riscv/boot/dts/starfive/jh7110-common.dtsi)。

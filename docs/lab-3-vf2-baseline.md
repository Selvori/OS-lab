# Lab 3：中断异常初步

本实验在 Lab 2 的物理页管理和 Sv39 内核页表基础上，建立 S-mode 陷阱处理框架，实现 UART 接收中断和基于 OpenSBI 的周期时钟中断。

详细实现与验证记录见 [Lab 3 详细实验报告](lab-3-report.md)。

## 实验目标

- 建立内核陷阱入口，完成整数寄存器现场的保存与恢复。
- 根据 scause 区分中断原因，调用对应处理函数。
- 配置 PLIC 和 UART，实现串口输入的中断回显。
- 通过 SBI 设置定时事件，使每个 CPU 接收周期时钟中断。
- 在双核环境下正确配合初始化、自旋锁和中断开关。

本实验不包含用户态陷阱、进程调度、软件 IPI 和异常恢复。无法处理的陷阱输出诊断信息后停止故障 CPU。

## 内核陷阱框架

每个 CPU 将自己的 stvec 设置为 kernel_vector，使用 Direct 模式。发生中断后，硬件记录 sepc、scause 等状态并关闭本核中断，再跳转到汇编入口。

硬件不会自动保存通用寄存器，因此 kernel_vector 在当前内核栈上建立 288 字节的陷阱帧：

| 偏移 | 保存内容 |
| --- | --- |
| 0–248 | x0–x31，其中 x0 槽为零，x2 保存原 sp |
| 256 | sstatus |
| 264 | sepc |
| 272 | scause |
| 280 | stval |

保存现场后，将陷阱帧地址传给 trap_kernel_handler。scause 最高位表示是否为中断，低位编号 5 和 9 分别对应 S-mode 时钟中断与外部中断。

处理结束后，汇编恢复 CSR 和整数寄存器，通过 sret 返回原执行流。恢复过程中保持 SIE=0，由 sret 根据 SPIE 恢复此前的中断状态。普通中断保留原 sepc，不统一增加 4。

当前陷阱帧沿用内核栈和页表，只保存整数上下文，不支持用户栈切换、浮点/向量状态或嵌套中断。

## 串口接收中断

PLIC 负责将外设中断路由到 CPU。启动核设置 UART 中断优先级，各 CPU 分别配置自己的 S-mode context，使能 UART 并设置响应阈值。

```text
输入字节 → UART FIFO → PLIC → kernel_vector
        → trap_kernel_handler → plic_claim
        → uart_intr → uart_getc → console_intr → printf
        → plic_complete → 恢复现场 → sret
```

plic_claim 读取待处理设备编号。若编号为 UART，则循环读取接收 FIFO，直到 uart_getc 返回 -1；字符经 console_intr 回显，最后执行 plic_complete，允许后续请求继续到达。双核竞争时 claim 可能返回 0，表示已无可领取的请求，此时直接返回。

UART 接收使用中断，发送仍采用轮询。uart_getc 返回 int，使有效字节 0–255 与无数据的 -1 区分。控制台将 CR 转换为换行，输出换行时发送 CRLF；当前没有行编辑和软件输入队列。

设备访问通过 asm/io.h 封装，使用 volatile 保证实际读写，并通过 fence 约束 UART 操作与 PLIC complete 的顺序。

## 时钟中断

内核运行在 S-mode，底层定时器由 OpenSBI 管理。每个 CPU 通过 SBI TIME 接口预约下一次事件，无须直接访问 CLINT，也不经过 PLIC。

```c
TIMER_INTERVAL = TIMEBASE_HZ / TIMER_HZ;
sbi_set_timer(csr_read(time) + TIMER_INTERVAL);
```

SBI 接收绝对到期时间。当前目标频率为 100 Hz：QEMU 的 time 计数频率为 10 MHz，对应间隔 100000；VisionFive2 为 4 MHz，对应间隔 40000。

```text
定时事件到期 → OpenSBI → S-mode 时钟中断
            → kernel_vector → trap_kernel_handler
            → timer_interrupt_handler
            → 预约下一次事件、本核 ticks 加一 → sret
```

周期性由每次处理后重新预约实现。timer_ticks(cpu) 查询指定 CPU 已处理的中断次数，采用原子操作支持跨核读取。发生处理延迟时，不补发错过的节拍，因此计数不能直接作为精确运行时间。

## 启动流程与多核同步

```text
_entry → start → arch_early_init → main
  启动核：print_init → page_allocator_init → kernel_vm_init
          → kernel_vm_activate → trap_kernel_inithart
          → plic_init → uart_rx_intr_enable
          → SBI HSM 启动从核 → release-store started
  从核：acquire-load started → kernel_vm_activate → trap_kernel_inithart
  每核：plic_inithart → timer_inithart
        → 设置 sie.SEIE/STIE → 开启 sstatus.SIE → arch_park
```

started 的 release/acquire 配合保证从核能够使用已完成的共享初始化结果。各 CPU 随后分别配置自己的陷阱入口、中断路由和定时事件，最后开启中断。

串口回显复用打印锁。自旋锁提供跨核互斥，push_off/pop_off 防止持锁期间发生本核中断重入。释放锁时先解锁再恢复中断；在中断处理器内调用 printf 不会提前开启中断。

## 代码结构

```text
arch/riscv64/kernel_vector.S       陷阱入口、现场保存与恢复
arch/riscv64/include/asm/trap.h     陷阱帧布局与偏移定义
arch/riscv64/trap.c                每核入口初始化与中断分发
arch/riscv64/timer.c               SBI 时钟预约与每核计数
arch/riscv64/boot/sbi.c            SBI 调用封装
arch/riscv64/include/asm/io.h       MMIO 访问与顺序约束
drivers/irq/plic.c                 PLIC 配置、claim 与 complete
drivers/serial/uart16550.c         UART 接收与发送
kernel/lib/console.c               控制台回显
kernel/main.c                      全局与每核初始化流程
```

## 构建和运行

```bash
cd ~/Work/Seele
make CONFIG=riscv64-qemu-virt-sbi build
make CONFIG=riscv64-qemu-virt-sbi run
```

退出 QEMU：先按 Ctrl-A，松开后按 X。

运行时两个 CPU 均输出时钟和串口就绪信息，输入字符后可以看到回显。启动核与输出先后顺序可能不同。

```text
cpu 0 SBI timer ready: 100 Hz
cpu 0 UART external interrupt route ready
cpu 1 SBI timer ready: 100 Hz
cpu 1 UART external interrupt route ready
```

## 实验结果

- QEMU 5.1.0 双核构建与运行通过，ELF 入口保持为 0x80200000。
- 两核均能够处理时钟中断，串口输入能够正常回显。
- 陷阱现场恢复、连续输入、多核并发和关中断后恢复测试通过。
- UART、时钟与打印、页分配同时运行时未发现本轮测试中的死锁或数据错误。
- 临时测试代码已恢复，正式版本重新构建运行通过。
- VisionFive2 V1.3B 已完成 FIT 手动启动、四核 Sv39、UART 回显和四核时钟计数验收（2026-10-20），详见下节。

## 平台说明

| 参数 | QEMU virt | VisionFive2/JH7110 |
| --- | --- | --- |
| UART IRQ | 10 | 32 |
| UART 访问宽度 | 8 位 | 32 位 |
| UART 寄存器间隔 | 1 字节 | 4 字节 |
| PLIC S-context | 2×hart+1 | 2×hart |
| time 计数频率 | 10 MHz | 4 MHz |

平台配置使用真实 hart ID 计算 PLIC context。当前 timebase 为平台常量，尚未解析设备树获取频率。同步回显适用于本次教学实验，未保证持续满速硬件输入下的无丢字能力。


## VisionFive2 上板验证（2026-10-20）

本次测试的是 Lab 3 完成版本 d9afed6，当前 lab-4 继承该基线，尚未增加 Lab 4 功能。开发板为 V1.3B / 4 GiB，SPI/QSPI 固件为 U-Boot 2021.10、OpenSBI v1.2；内核当前仅管理前 128 MiB DRAM。

在 Ubuntu 虚拟机 ~/Work/Seele 构建：

```bash
make CONFIG=riscv64-visionfive2-uboot build image
dumpimage -l build/riscv64-visionfive2-uboot/kernel.itb
```

image 需要 u-boot-tools 提供的 mkimage 和 device-tree-compiler 提供的 dtc。mk/image.mk 已改为 mkimage -f 打包，并把规则文件列入依赖；原来直接 dtc 输出的容器缺少根 timestamp，dumpimage 将其识别为 GP Header。实际使用 mkimage 产物后，U-Boot iminfo/bootm 识别与启动通过。FIT 中的 os=linux 用于 U-Boot 的 RISC-V S-mode 交接，不代表 Seele 使用 Linux 内核接口。

将镜像作为独立文件放入板端 /boot，保留 Ubuntu 的内核、initrd 和 DTB。当前部署文件是 /boot/seele-d9afed6-20261020.itb，SHA-256 为 41324422d35fb3123a232270507eceb656507a3096e13e0261ec51bb990b7eec。FIT 时间戳会使重新打包的容器哈希改变；本轮恢复构建后的 kernel.bin 与测试前逐字节相同，SHA-256 为 c32398724c9c37b71e0a2cadcb7220dfffa9a8cce03773197c551256e5c4ba54。

Ubuntu 运行时先 sudo systemctl reboot，再截停 U-Boot 的两秒倒计时。在 StarFive # 提示符逐条执行，确认每次加载成功后继续：

```text
mmc dev 1
load mmc 1:3 0x50000000 /seele-d9afed6-20261020.itb
iminfo 0x50000000
load mmc 1:3 0x51000000 /dtbs/6.12.5-starfive/starfive/jh7110-starfive-visionfive-2-v1.3b.dtb
bootm 0x50000000 - 0x51000000
```

FIT 暂存地址、DTB 地址和内核装载/入口地址 0x40200000 分开。本机 bdinfo 已核对 4 GiB RAM 和高端 U-Boot 重定位；换板、换镜像或扩大内核管理范围后需重新核对内存布局。

硬件结果：

- CPU0–3 均启用同一 Sv39 根页表 0x40214000，四核 TIME 和 UART 路由初始化完成。
- 正式版本两次通过 1153 字节输入的逐字节回显测试：32 字节/25ms 分批，含 NUL、DEL、高位字节及 CR/LF；按换行规则输出 1155 字节。空闲一秒后继续回显通过。
- 临时探针按 time CSR 约一秒间隔采样三次，CPU0/1 为 100→201→302，CPU2 为 101→202→303，CPU3 为 101→203→304，证明各核时钟中断持续处理；不是精密频率或持续满速输入测试。
- 探针 main.c 已逐字节恢复并重新构建，正式 kernel.bin 与插入前相同。正式镜像重新从 SD 启动并再次通过回显，最后运行的是正式版本。
- bootcmd 保持 run vf2_sdboot，下一次未截停的启动仍进入 Ubuntu。Seele 当前只提供中断回显，没有登录 shell；运行本版本 Seele 时可按 RST 返回启动流程。Ubuntu 运行时使用正常关机/重启命令。

限制与过程记录：VMware 内 CH340 出现 -110 控制超时，改用 Windows COM7 后可重复通信，透传根因尚未修复。首轮探针误用了只返回 a0 error 的 sbi_call 封装进行能力判断，导致临时 PANIC；修正后计数再次通过。随后 SBI SRST 尝试报告 cannot read pmic power register，自动复位未成功，最终经用户物理复位恢复正式内核；没有修改固件或为正式内核添加复位接口。

硬件日志及测试辅助文件保留在忽略目录 build/vf2-bringup-20261020/。Windows 资料目录另有本次上板记录和原始证据。

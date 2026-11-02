# Lab 4 实验报告

日期：2026-11-02。代码仓库：Ubuntu `/home/selvori/Work/Seele`，分支 `lab-4`。起始提交 `d9afed6246e608f5e47185f4ca6ae2adff8380be`。

## 目标与依据

依据 Lab 4 课件全部 9 页，在现有 OpenSBI/Sv39/中断框架上建立首个用户进程，理解 PCB、虚拟地址空间、context 和 trapframe 的职责，完成真实 U-mode、ecall 进入内核和 sret 返回的闭环。

配套课程 YAML 未提供，用户明确同意按现有接口自建规格。先编写 design.yaml、四个 module 契约和 patches/lab4.yaml，再以 `e0a8bbd` 签署提交；实现与测试遵循这些合同。write/getpid/ticks/exit 的编号由本项目定义，并非课件指定。

## 实现原理

PCB 的 pgtbl 指向私有 Sv39 根，tf 指向独立物理页，ctx 为 ra/sp/s0–s11。首次 ctx.ra 设为 trap_user_return，ctx.sp 为进程内核栈顶；proc_run 保存启动上下文后，swtch 恢复该初始上下文。用户程序独立链接到 0x1000，以裸二进制嵌入只读内核映像，创建时复制到用户代码页，并执行 fence.i。

用户与内核页表在 `TRAMPOLINE` 将同一物理代码页映射为 supervisor RX。用户 trap 发生时，sscratch 提供 supervisor trapframe 地址，汇编保存用户整数寄存器，再装入可信的内核 sp/gp/tp/satp。切换页表前后使用 sfence.vma，随后跳入内核 C handler。用户传入任意 gp/tp 都不能改变内核 CPU 身份。

C handler 立即恢复 kernel_vector，保存 sepc，断言 SPP=0 且 SIE=0。ecall 仅对该指令将 epc 增加 4；设备中断复用 Lab 3 的服务函数，保留 epc。返回时清除 SPP/SUM/MXR/FS/VS，置 SPIE，设置 sepc/stvec，最后进入共享 user_return，在用户页表下恢复现场并 sret。

用户页只有代码 RXU、数据 RWU 和一页栈 RWU。trapframe、trampoline 和进程内核栈 U=0；空页、内核 DRAM/MMIO、内核栈下方 guard 保持未映射。内核页表保留物理恒等映射，copyin 根据私有 PTE 验证 U/R 后读取相应物理别名，不开启 SUM。

进程固定在启动核运行。内核高地址页表节点在启动从核前随 trampoline 建立，后续只修改 owner 核专用栈叶子；其他核不访问该 VA，不存在迁移或远程使用此栈的场景。因此本阶段不引入通用 TLB shootdown；未来多进程/迁移必须重新设计该约束。

进程退出或用户故障都先 swtch 回启动栈，再清除内核栈叶子并刷新本核 TLB，释放私有页表节点及所有进程物理页。共享 trampoline 物理页和共享内核页表节点不释放。创建失败同样逐项回滚；正式启动中的创建失败由 main 明确 panic。

## 实施过程

1. 发现虚拟机未运行，通过现有 vmrun 启动，确认 SSH、lab-4 分支、个人 origin 和既有 README/FIT 改动。Lab 3 基线构建运行通过。
2. 阅读完整课件、地址图、项目规范和真实接口，编写并提交规格。
3. 增加 PCB、用户表/copyin、swtch、用户 trapframe/trampoline、syscall、用户镜像构建与启动连接。
4. 修正用户汇编前向长度表达式与构建列表位置。首轮运行因演示程序硬编码字符串长度错误出现 USER FAIL，改为和传入长度比较后通过。
5. 编写主机验收脚本，分别运行正式、owner UART、十类故障、失败注入和恢复回归。长串测试首次用重复后缀截取导致脚本提前匹配，改为精确字节计数并重跑通过。
6. 临时源码在 finally 中逐字节恢复，核对 SHA-256 并重建；完成正式运行、双核 ticks、VF2 镜像及原生 QEMU 截图。

## 验收与实际结果

命令：

```bash
make CONFIG=riscv64-qemu-virt-sbi build
python3 tools/test_lab4.py --extended
make CONFIG=riscv64-visionfive2-uboot build image
dumpimage -l build/riscv64-visionfive2-uboot/kernel.itb
```

所有编译使用现有 -Wall -Wextra -Werror。QEMU/VF2 ELF 入口为 0x80200000/0x40200000。FIT 被 dumpimage 识别为 RISC-V kernel，load/entry 均为 0x40200000。

| 场景 | 结果 |
| --- | --- |
| 正式 U-mode 4096 次 getpid | 返回 pid=1，所有被检查非返回寄存器、用户数据/栈保持正确 |
| 未知 syscall 和 write 参数 | -38/-9/-14/-22 符合合同；无部分输出 |
| 正式恢复回归 | exit 0，syscalls=208845，user-timer=102，user-external=6 |
| owner-only UART 路由 | 用户来源外部中断 16 次，标记全部准确回显 |
| 退出后 UART | 5777 输入字节逐字节符合回显规则，含 4096 长串、二进制字节及空闲后恢复 |
| 用户读取空页/内核地址/tf/kstack/trampoline | scause=13，用户进程 faulted，内核继续回显 |
| 用户写代码/用户栈溢出 | scause=15，结束故障进程并回收 |
| 用户执行栈 | scause=12，结束故障进程并回收 |
| 用户读取 S-mode CSR/非法指令 | scause=2，证实特权隔离，内核继续运行 |
| 分配失败 | 10 个分配调用逐点失败，覆盖中间页表，两个页池计数恢复 |
| 重复创建/销毁 | 32 轮通过，计数恢复 |
| 页表和 copyin | 精确权限、空页/MMIO/DRAM/guard 隔离通过；合法跨页成功、越界/跨 tf 拒绝且目标不变 |
| 退出后双核时钟 | GDB ticks [171,171]→[211,211]，不是精密频率测试 |
| 临时测试恢复 | main/page/proc/init 原始字节 SHA-256 一致；正式 ELF 无 lab4_test/lab4_fail/lab4_allocation |

最终又对两平台执行 make -B 全量重建，并运行正式回归：exit=0、syscalls=203455、user-timer=103、user-external=5，退出后 5777 字节回显通过。测试覆盖启动 hart 0 和 1。此轮证据单独保存为 final-recheck-results.json 与 final-recheck-serial.log。

各次验收脚本启动的 make run 通过 Ctrl-A/X 正常退出，返回 0。初始基线/联调 timeout 结束的运行单独保留在忽略目录，未与正常退出证据混淆。

精选输出和汇总在 [lab-4-evidence](lab-4-evidence/)，完整过程日志在 build/lab4-20261102 和 build/lab4-checks。串口证据归档为 LF，原始字节仍在 build 中。截图为 QEMU serial vc 的 monitor screendump 原生画面，PPM 只转换为 PNG，未重新绘制日志。

## 范围与后续接口

Lab 4 已验证 QEMU 用户态及 VF2 ELF/FIT 构建；未部署或验证 Lab 4 开发板用户态。Lab 3 上板结果独立归档，不混用。

本阶段没有调度器或并发进程，进程不迁移；上下文只包含整数寄存器，用户 FS/VS 禁用。系统调用有界且在关中断期间处理，无嵌套中断。heap_top 只记录初始堆起点，不提供扩堆。后续可在 PCB/私有页表/系统调用分派上扩展进程调度和用户内存管理，但需要补充同步、TLB 失效与更完整的资源生命周期合同。

规格对“启动前发布页表”的表述在实现验收中细化为“共享表拓扑在启动前发布，专用栈叶子只由 owner 使用”；实际节点分配与回收测试证明该阶段页计数可恢复。

既有 README 的上板内容完整归档于 [Lab 3/VF2 基线](lab-3-vf2-baseline.md)，mk/image.mk 保持本轮开始时已验证的 mkimage 修复内容。

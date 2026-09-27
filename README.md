# Lab 2：物理页管理与 Sv39 内核页表

本分支在 Lab 1 的启动、串口输出和自旋锁基础上，实现 4 KiB 物理页分配器、Sv39 三级页表和所有 CPU 共享的内核页表。

## 实验目标

- 将可分配 DRAM 划分为内核页池和用户页池。
- 使用带锁单链表完成物理页的常数时间分配与释放。
- 实现 Sv39 页表遍历、映射、查询和 `satp` 切换。
- 使用恒等映射保护内核代码、只读数据和可写数据。
- 让每个 CPU 分别启用同一张内核页表。

本实验尚不包含用户进程、用户页表、缺页处理、大页、交换、COW、ASID 和页表销毁。

## 物理内存布局

QEMU 使用 `0x80000000` 开始的 128 MiB DRAM：

| 区域 | 范围 | 用途 |
| --- | --- | --- |
| OpenSBI | `0x80000000` 至 `0x80200000` | 固件保留，不映射也不分配 |
| 内核映像 | `0x80200000` 至 `kernel_end` | 代码、只读数据、数据、BSS 和启动栈 |
| 内核页池 | `PGROUNDUP(kernel_end)` 后 25% 的可用页 | 页表页和内核动态页 |
| 用户页池 | 剩余 75% 的可用页 | 为后续用户进程保留 |
| DRAM 上界 | `0x88000000` | 不可分配的排他上界 |

两个页池各自保存空闲链表、空闲页计数和自旋锁。空闲页本身的第一个机器字用于保存下一节点地址，因此分配器不依赖堆或额外元数据区。

`page_alloc()` 从指定页池移除链表头并将整个页面清零；`page_free()` 检查页对齐和地址范围，以 `0xa5` 填充页面后放回所属页池。页表页只从内核页池分配。

## Sv39 页表

Sv39 将低 39 位虚拟地址划分为三级 9 位 VPN 和 12 位页内偏移。当前实现只使用低地址规范区和 4 KiB 叶子页：

```text
VPN[2]  VPN[1]  VPN[0]  OFFSET
 9 bit   9 bit   9 bit   12 bit
```

`sv39_walk()` 按 VPN[2]、VPN[1]、VPN[0] 遍历三级页表，并按需从内核页池申请中间页表。`sv39_map()` 拒绝未对齐范围、重复映射、越界地址以及只有 W 没有 R 的非法权限组合。叶子页预置 A 位，可写页同时预置 D 位，使内核在尚无缺页处理时也能运行。

## 内核映射

内核使用虚拟地址等于物理地址的恒等映射：

| 区域 | 权限 |
| --- | --- |
| UART | R/W |
| PLIC | R/W |
| `.text` | R/X |
| `.rodata` | R |
| `.data`、BSS、启动栈和其余 DRAM | R/W |
| OpenSBI 固件区域 | 未映射 |
| CLINT | 未映射，由 OpenSBI 管理 |

链接脚本导出页对齐的 `kernel_text_start`、`kernel_text_end`、`kernel_rodata_start`、`kernel_rodata_end`、`kernel_data_start` 和 `kernel_end`。建立页表时会用软件查询逐项检查恒等关系和权限，并确认 OpenSBI、CLINT 地址没有映射。

## 启动流程

```text
_entry
  -> start
      -> arch_early_init：保持 satp 为 Bare
      -> main
          -> 启动核
              -> print_init
              -> page_allocator_init
              -> kernel_vm_init
              -> kernel_vm_activate
              -> SBI HSM 启动其他 CPU
              -> release-store started
          -> 从核
              -> acquire-load 等待 started
              -> kernel_vm_activate
          -> arch_park
```

`kernel_vm_init()` 只由启动核执行。每个 CPU 都会把共享根页表的物理页号写入自己的 `satp`，随后执行 `sfence.vma`。恒等映射保证切换前后的 PC、栈、全局变量以及传给 OpenSBI 的从核入口地址保持不变。

## 代码结构

```text
kernel/mm/page.c                 物理页池、分配和释放
kernel/mm/vm.c                   内核映射策略和运行时校验
arch/riscv64/mm/sv39.c          Sv39 walk、map、lookup 和 satp
arch/riscv64/include/asm/sv39.h PTE、VPN 和 satp 定义
kernel/lib/string.c             freestanding memset
```

## 构建和运行

```bash
cd ~/Work/Seele
make CONFIG=riscv64-qemu-virt-sbi build
make CONFIG=riscv64-qemu-virt-sbi run
```

退出 QEMU：先按 `Ctrl-A`，松开后按 `X`。

一次实际运行输出如下。OpenSBI 选择的启动 hart 可能不是 CPU 0，因此两条 CPU 信息的顺序不固定。

```text
cpu 1 is booting!
memory: kernel 0x000000008020c000-0x0000000082189000, 8061 free pages
memory: user   0x0000000082189000-0x0000000088000000, 24183 free pages
cpu 1 enabled Sv39 with root 0x000000008020c000
cpu 0 enabled Sv39 with root 0x000000008020c000
```

两个 CPU 输出相同的根页表地址，说明它们分别启用了同一张共享内核页表。

## 验证记录

- QEMU 版本：5.1.0。
- QEMU 配置使用 `-m 128M -smp 2 -bios default`。
- QEMU 内核构建在 `-Wall -Wextra -Werror` 下通过。
- ELF 入口保持为 `0x80200000`，各链接边界均为 4 KiB 对齐。
- 双核在 Sv39 模式下继续执行和串口输出，无 `PANIC`。
- 临时双核压力测试让每个 CPU 对两个页池执行 512 轮、每轮 16 页的并发分配和释放；页面对齐、范围、唯一性、全页清零和最终计数恢复均通过。测试代码验证后已删除。
- 临时耗尽测试分别取空两个页池，确认空池返回 `NULL`，随后释放全部页面并恢复原计数。测试代码验证后已删除。
- 临时向 `.text` 页写入并用 GDB 捕获 `early_trap`，确认 `scause=15`、`satp=0x800000000008020c`，硬件实际执行了 Sv39 的 R/X 写保护；测试代码验证后已删除。
- VisionFive2 内核 ELF 构建通过；生成 `kernel.itb` 所需的 `dtc` 当前未安装，因此镜像打包和开发板运行尚未验证。

## 平台说明

QEMU 5.1.0 的 `virt` 机器把 PLIC 放在 `0x0c000000`，区域长度为 `0x04000000`。VisionFive2 的 JH7110 设备树使用相同的 PLIC 和 CLINT 基址；实际开发板运行仍需在安装 `dtc`、生成 FIT 镜像后验证。

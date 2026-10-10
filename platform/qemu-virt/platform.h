#ifndef OSLAB_PLATFORM_H
#define OSLAB_PLATFORM_H
#define PLATFORM_NAME "qemu-virt"
#define HART_FIRST 0
/* time CSR 的计数频率，不是 CPU 主频或 UART 时钟。 */
#define TIMEBASE_HZ 10000000UL
#define NCPU 2
#define DRAM_BASE 0x80000000UL
#define DRAM_SIZE (128UL * 1024 * 1024)
#define DRAM_END (DRAM_BASE + DRAM_SIZE)
#define UART_BASE 0x10000000UL
#define UART_CLOCK 3686400
#define UART_SHIFT 0
#define PLIC_BASE 0x0c000000UL
#define PLIC_SIZE 0x04000000UL
#define CLINT_BASE 0x02000000UL
#define CLINT_SIZE 0x00010000UL
#define UART_IRQ 10
#define UART_IO_WIDTH 1
#define PLIC_MAX_IRQ 127
/* QEMU 每个 hart 都有 M、S 两个 context。 */
#define PLIC_S_CONTEXT(hart) (2UL * (hart) + 1)
#endif

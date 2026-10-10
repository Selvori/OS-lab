#ifndef OSLAB_PLATFORM_H
#define OSLAB_PLATFORM_H
#define PLATFORM_NAME "visionfive2"
#define HART_FIRST 1
/* time CSR 的计数频率，不是 CPU 主频或 UART 时钟。 */
#define TIMEBASE_HZ 4000000UL
#define NCPU 4
#define DRAM_BASE 0x40000000UL
#define DRAM_SIZE (128UL * 1024 * 1024)
#define DRAM_END (DRAM_BASE + DRAM_SIZE)
#define UART_BASE 0x10000000UL
#define UART_CLOCK 24000000
#define UART_SHIFT 2
#define PLIC_BASE 0x0c000000UL
#define PLIC_SIZE 0x04000000UL
#define CLINT_BASE 0x02000000UL
#define CLINT_SIZE 0x00010000UL
#define UART_IRQ 32
#define UART_IO_WIDTH 4
#define PLIC_MAX_IRQ 136
/* JH7110 hart 0 只有 M-context，hart 1 的 S-context 编号为 2。 */
#define PLIC_S_CONTEXT(hart) (2UL * (hart))
#endif

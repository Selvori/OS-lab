/* 16550：轮询发送，接收由中断驱动。MMIO 宽度由平台指定。 */
#include "uart16550.h"
#include <asm/io.h>
#include <kernel/console.h>
#include <platform.h>

#define UART_RHR 0
#define UART_THR 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_LSR 5
#define IER_RX 0x01
#define LSR_RX_READY 0x01
#define LSR_TX_IDLE 0x20

static uint8 read_reg(uint64 base, unsigned shift, unsigned reg)
{
    uint64 address = base + ((uint64)reg << shift);
#if UART_IO_WIDTH == 4
    return (uint8)mmio_read32(address);
#elif UART_IO_WIDTH == 1
    return mmio_read8(address);
#else
#error Unsupported UART register access width
#endif
}

static void write_reg(uint64 base, unsigned shift, unsigned reg, uint8 value)
{
    uint64 address = base + ((uint64)reg << shift);
#if UART_IO_WIDTH == 4
    mmio_write32(address, value);
#else
    mmio_write8(address, value);
#endif
}

void uart_init(uint64 base, unsigned clock, unsigned shift)
{
    unsigned divisor = (clock + 8 * 115200) / (16 * 115200);
    /* 此时陷阱系统还未就绪；RX 中断由后续初始化单独开启。 */
    write_reg(base, shift, UART_IER, 0);
    write_reg(base, shift, UART_LCR, 0x80);
    write_reg(base, shift, 0, divisor & 0xff);
    write_reg(base, shift, 1, divisor >> 8);
    /* 8N1；清空/开启 FIFO，接收触发阈值为 1 字节。 */
    write_reg(base, shift, UART_LCR, 3);
    write_reg(base, shift, UART_FCR, 7);
}

void uart_rx_intr_enable(uint64 base, unsigned shift)
{
    /* 只使能接收；发送仍轮询，不能开启尚无处理器的 TX 中断。 */
    write_reg(base, shift, UART_IER, IER_RX);
}

void uart_putc(uint64 base, unsigned shift, uint8 c)
{
    while ((read_reg(base, shift, UART_LSR) & LSR_TX_IDLE) == 0) {}
    write_reg(base, shift, UART_THR, c);
}

int uart_getc(uint64 base, unsigned shift)
{
    /* 没有数据立即返回 -1；0..255 都是有效字节，包括 NUL。 */
    if (!(read_reg(base, shift, UART_LSR) & LSR_RX_READY))
        return -1;
    return read_reg(base, shift, UART_RHR);
}

void uart_intr(uint64 base, unsigned shift)
{
    int c;
    /* 排空当前可读 FIFO，同时消除接收/接收超时中断原因。 */
    while ((c = uart_getc(base, shift)) >= 0)
        console_intr(c);
}

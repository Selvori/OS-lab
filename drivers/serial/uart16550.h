#ifndef OSLAB_UART_H
#define OSLAB_UART_H
#include <kernel/types.h>
void uart_init(uint64 base, unsigned clock, unsigned shift);
void uart_putc(uint64 base, unsigned shift, uint8 c);
void uart_rx_intr_enable(uint64 base, unsigned shift);
int uart_getc(uint64 base, unsigned shift);
void uart_intr(uint64 base, unsigned shift);
#endif

#include <kernel/console.h>
#include <drivers/serial/uart16550.h>
#include <asm/sbi.h>
#include <platform.h>
#include <kernel/print.h>
void console_init(void) { uart_init(UART_BASE, UART_CLOCK, UART_SHIFT); }
void console_putc(char c)
{
    if (c == '\n') uart_putc(UART_BASE, UART_SHIFT, '\r');
    uart_putc(UART_BASE, UART_SHIFT, (uint8)c);
}
void emergency_puts(const char *s)
{
    while (*s) { if (*s == '\n') sbi_putchar('\r'); sbi_putchar(*s++); }
}

/* 此阶段仅回显，不提供行编辑或供进程读取的输入队列。 */
void console_intr(int c)
{
    /* Enter 常产生 CR；统一交给 console_putc 输出 CRLF。 */
    printf("%c", c == '\r' ? '\n' : c);
}

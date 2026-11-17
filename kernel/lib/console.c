#include <kernel/console.h>
#include <drivers/serial/uart16550.h>
#include <asm/sbi.h>
#include <platform.h>
#include <kernel/print.h>
#include <kernel/file.h>
#include <kernel/lock.h>
#include <kernel/proc.h>
enum { RX_SIZE = 512 };
static uint8 rx[RX_SIZE];
static unsigned head, tail;
static bool input_enabled;
static spinlock_t rx_lock;

void console_init(void)
{
    spinlock_init(&rx_lock, "console RX");
    uart_init(UART_BASE, UART_CLOCK, UART_SHIFT);
}
void console_putc(char c)
{
    if (c == '\n') uart_putc(UART_BASE, UART_SHIFT, '\r');
    uart_putc(UART_BASE, UART_SHIFT, (uint8)c);
}
void emergency_puts(const char *s)
{
    while (*s) { if (*s == '\n') sbi_putchar('\r'); sbi_putchar(*s++); }
}

void console_input_enable(void)
{
    spinlock_acquire(&rx_lock);
    input_enabled = true;
    spinlock_release(&rx_lock);
}

void console_intr(int c)
{
    spinlock_acquire(&rx_lock);
    bool queued = input_enabled;
    if (queued && tail - head < RX_SIZE)
        rx[(tail++) % RX_SIZE] = (uint8)c;
    spinlock_release(&rx_lock);

    if (!queued)
        printf("%c", c == '\r' ? '\n' : c);
}

int console_read(void *data, size_t count)
{
    if (count == 0)
        return 0;
    size_t n = 0;
    for (;;) {
        spinlock_acquire(&rx_lock);
        bool enabled = input_enabled;
        while (head != tail && n < count)
            ((uint8 *)data)[n++] = rx[(head++) % RX_SIZE];
        spinlock_release(&rx_lock);

        if (!enabled)
            return -19;
        if (n != 0)
            return (int)n;
        /* Either hart can enqueue. Retry on the owner's next clock tick. */
        proc_sleep_ticks(1);
    }
}

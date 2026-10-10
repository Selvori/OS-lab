#include <asm/csr.h>
#include <asm/trap.h>
#include <kernel/arch.h>
#include <kernel/console.h>
#include <kernel/print.h>
#include <kernel/trap.h>
#include <kernel/timer.h>
#include <drivers/irq/plic.h>
#include <drivers/serial/uart16550.h>
#include <platform.h>

/* 故障可能发生在持有 print_lock 时，诊断只能走无锁紧急输出。 */
static void trap_hex(const char *name, uint64 value)
{
    static const char digits[] = "0123456789abcdef";
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (unsigned i = 0; i < 16; ++i)
        text[2 + i] = digits[(value >> (60 - 4 * i)) & 15];
    text[18] = '\0';
    emergency_puts(name);
    emergency_puts(text);
    emergency_puts("\n");
}

static void trap_fatal(const struct kernel_trapframe *frame, const char *reason)
    __attribute__((noreturn));

static void trap_fatal(const struct kernel_trapframe *frame, const char *reason)
{
    arch_irq_disable();
    emergency_puts("\nKERNEL TRAP: ");
    emergency_puts(reason);
    emergency_puts("\n");
    trap_hex("hart=", arch_hart_id());
    trap_hex("scause=", frame->scause);
    trap_hex("sepc=", frame->sepc);
    trap_hex("stval=", frame->stval);
    trap_hex("sstatus=", frame->sstatus);
    trap_hex("sp=", frame->x[2]);
    panic("unhandled kernel trap");
}

void trap_kernel_inithart(void)
{
    extern void kernel_vector(void);
    arch_irq_disable();
    csr_write(sie, 0);
    /* Direct 模式：中断和异常都从同一汇编入口进入，低两位为 0。 */
    csr_write(stvec, (uint64)kernel_vector);
}

static void external_interrupt_handler(struct kernel_trapframe *frame)
{
    uint32 irq = plic_claim();
    /* 通知可能已经被其他 hart 领取；0 不是需要 complete 的 IRQ。 */
    if (irq == 0)
        return;
    if (irq == UART_IRQ) {
        uart_intr(UART_BASE, UART_SHIFT);
        plic_complete(irq);
        return;
    }
    plic_complete(irq);
    trap_hex("unexpected PLIC irq=", irq);
    trap_fatal(frame, "unknown external interrupt source");
}

void trap_kernel_handler(struct kernel_trapframe *frame)
{
    /* SPP 是陷入前的特权级；SIE 在硬件陷入时应已清零。 */
    if (!(frame->sstatus & SSTATUS_SPP) || arch_irq_enabled())
        trap_fatal(frame, "invalid kernel trap state");

    if (!(frame->scause & SCAUSE_INTERRUPT))
        trap_fatal(frame, "synchronous exception");

    /* 时钟与 UART 已接入；软件中断仍保持关闭。 */
    switch (frame->scause & ~SCAUSE_INTERRUPT) {
    case IRQ_S_SOFT:
        trap_fatal(frame, "software interrupt not installed");
    case IRQ_S_TIMER:
        timer_interrupt_handler();
        return;
    case IRQ_S_EXT:
        external_interrupt_handler(frame);
        return; /* 普通中断保持 sepc 不变，由汇编恢复并 sret。 */
    default:
        trap_fatal(frame, "unknown interrupt");
    }
}

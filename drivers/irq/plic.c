#include "plic.h"
#include <asm/io.h>
#include <kernel/arch.h>
#include <kernel/print.h>
#include <platform.h>

#define PLIC_ENABLE_BASE   0x2000UL
#define PLIC_ENABLE_STRIDE 0x80UL
#define PLIC_CONTEXT_BASE  0x200000UL
#define PLIC_CONTEXT_STRIDE 0x1000UL

_Static_assert(UART_IRQ > 0 && UART_IRQ <= PLIC_MAX_IRQ, "UART IRQ range");

static uint64 context_base(void)
{
    return PLIC_BASE + PLIC_CONTEXT_BASE +
           PLIC_S_CONTEXT(arch_hart_id()) * PLIC_CONTEXT_STRIDE;
}

void plic_init(void)
{
    /* 0 表示禁用该源；UART 使用最低的非零优先级 1。 */
    mmio_write32(PLIC_BASE + 4UL * UART_IRQ, 1);
}

void plic_inithart(void)
{
    uint64 enable = PLIC_BASE + PLIC_ENABLE_BASE +
                    PLIC_S_CONTEXT(arch_hart_id()) * PLIC_ENABLE_STRIDE;
    /* 清理本核 S-context 的固件残留，只开放 UART，不改 M-context。 */
    for (unsigned word = 0; word <= PLIC_MAX_IRQ / 32; ++word)
        mmio_write32(enable + 4UL * word, 0);
    mmio_write32(enable + 4UL * (UART_IRQ / 32), 1U << (UART_IRQ % 32));
    /* 只有优先级严格大于阈值的中断才能通知此 context。 */
    mmio_write32(context_base(), 0);
}

uint32 plic_claim(void)
{
    /* 读取有副作用：原子领取一个中断。返回 0 表示当前没有可领取的源。 */
    return mmio_read32(context_base() + 4);
}

void plic_complete(uint32 irq)
{
    assert(irq > 0 && irq <= PLIC_MAX_IRQ, "PLIC complete invalid IRQ");
    /* 与 claim 使用同一个地址；在设备原因消除之后通知网关处理完毕。 */
    mmio_write32(context_base() + 4, irq);
}

#include <asm/csr.h>
#include <asm/sbi.h>
#include <kernel/arch.h>
#include <kernel/print.h>
#include <kernel/timer.h>
#include <platform.h>

#define TIMER_INTERVAL (TIMEBASE_HZ / TIMER_HZ)
_Static_assert(TIMEBASE_HZ >= TIMER_HZ && TIMEBASE_HZ % TIMER_HZ == 0,
               "timer frequency must divide platform timebase");

static uint64 ticks[NCPU];

static void timer_rearm(void)
{
    /* SBI 接收绝对 time 值。以当前时刻重设，延迟后不补发旧节拍。 */
    if (sbi_set_timer(csr_read(time) + TIMER_INTERVAL) != 0)
        panic("SBI TIME set_timer failed");
}

void timer_inithart(void)
{
    assert(!arch_irq_enabled(), "timer init requires interrupts disabled");
    timer_rearm();
}

void timer_interrupt_handler(void)
{
    /* 重新预约同时由固件清除当前 pending；时钟中断不经过 PLIC。 */
    timer_rearm();
    __atomic_fetch_add(&ticks[arch_cpu_id()], 1, __ATOMIC_RELAXED);
}

uint64 timer_ticks(uint64 cpu)
{
    assert(cpu < NCPU, "invalid timer cpu");
    return __atomic_load_n(&ticks[cpu], __ATOMIC_RELAXED);
}

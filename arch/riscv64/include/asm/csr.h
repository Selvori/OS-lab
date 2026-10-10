#ifndef OSLAB_CSR_H
#define OSLAB_CSR_H
#include <kernel/types.h>
#define csr_read(reg) ({ uint64 x; __asm__ volatile("csrr %0, " #reg : "=r"(x)); x; })
#define csr_write(reg, x) __asm__ volatile("csrw " #reg ", %0" :: "r"((uint64)(x)) : "memory")
#define SSTATUS_SIE  (1UL << 1)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SPP  (1UL << 8)
#define SCAUSE_INTERRUPT (1UL << 63)
#define IRQ_S_SOFT  1
#define IRQ_S_TIMER 5
#define IRQ_S_EXT   9
#define SIE_STIE (1UL << IRQ_S_TIMER)
#define SIE_SEIE (1UL << IRQ_S_EXT)

static inline void sfence_vma(void)
{
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}
#endif

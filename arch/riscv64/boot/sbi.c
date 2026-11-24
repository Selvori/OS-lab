/* S-mode 调用固件。 */
#include <asm/sbi.h>
#include <kernel/arch.h>
#include <platform.h>
long sbi_call(uint64 extension, uint64 function, uint64 x, uint64 y, uint64 z)
{
    register uint64 a0 __asm__("a0") = x;
    register uint64 a1 __asm__("a1") = y;
    register uint64 a2 __asm__("a2") = z;
    register uint64 a6 __asm__("a6") = function;
    register uint64 a7 __asm__("a7") = extension;
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a6), "r"(a7) : "memory");
    return (long)a0;
}
/* SBI v0.2 TIME，RV64 在 a0 中传递完整的 64 位截止时间。 */
long sbi_set_timer(uint64 deadline)
{
    return sbi_call(0x54494d45, 0, deadline, 0, 0);
}
#ifdef DUAL_HART_USERS
long sbi_send_ipi(uint64 hart)
{
    /* SBI v0.2 IPI: select bit zero with the target hart as the mask base. */
    return sbi_call(0x735049, 0, 1, hart, 0);
}
#endif
void sbi_putchar(char c) { (void)sbi_call(1, 0, (uint8)c, 0, 0); }
int arch_start_cpu(uint64 cpu)
{
    extern void secondary_entry(void);
    if (cpu >= NCPU || cpu == arch_cpu_id()) return -1;
    return (int)sbi_call(0x48534d, 0, cpu + HART_FIRST, (uint64)secondary_entry, 0);
}

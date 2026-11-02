#include <asm/usertrap.h>
.section .trampoline,"ax",@progbits
.balign 4096
.globl trampoline, user_vector, user_return
trampoline:
user_vector:
    # sscratch is the supervisor-only trapframe VA; never trust user sp/tp/gp.
    csrrw a0, sscratch, a0
    sd zero, UTF_X(0)(a0)
    sd ra, UTF_X(1)(a0)
    sd sp, UTF_X(2)(a0)
    sd gp, UTF_X(3)(a0)
    sd tp, UTF_X(4)(a0)
    sd t0, UTF_X(5)(a0)
    sd t1, UTF_X(6)(a0)
    sd t2, UTF_X(7)(a0)
    sd s0, UTF_X(8)(a0)
    sd s1, UTF_X(9)(a0)
    sd a1, UTF_X(11)(a0)
    sd a2, UTF_X(12)(a0)
    sd a3, UTF_X(13)(a0)
    sd a4, UTF_X(14)(a0)
    sd a5, UTF_X(15)(a0)
    sd a6, UTF_X(16)(a0)
    sd a7, UTF_X(17)(a0)
    sd s2, UTF_X(18)(a0)
    sd s3, UTF_X(19)(a0)
    sd s4, UTF_X(20)(a0)
    sd s5, UTF_X(21)(a0)
    sd s6, UTF_X(22)(a0)
    sd s7, UTF_X(23)(a0)
    sd s8, UTF_X(24)(a0)
    sd s9, UTF_X(25)(a0)
    sd s10, UTF_X(26)(a0)
    sd s11, UTF_X(27)(a0)
    sd t3, UTF_X(28)(a0)
    sd t4, UTF_X(29)(a0)
    sd t5, UTF_X(30)(a0)
    sd t6, UTF_X(31)(a0)
    csrr t0, sscratch
    sd t0, UTF_X(10)(a0)
    ld sp, UTF_KSP(a0)
    ld tp, UTF_HART(a0)
    ld gp, UTF_GP(a0)
    ld t0, UTF_KSATP(a0)
    ld t1, UTF_HANDLER(a0)
    sfence.vma zero, zero
    csrw satp, t0
    sfence.vma zero, zero
    jr t1
.balign 4
user_return:
    # a0=trapframe VA, a1=user satp. All following instructions share the same VA.
    sfence.vma zero, zero
    csrw satp, a1
    sfence.vma zero, zero
    csrw sscratch, a0
    ld ra, UTF_X(1)(a0)
    ld sp, UTF_X(2)(a0)
    ld gp, UTF_X(3)(a0)
    ld tp, UTF_X(4)(a0)
    ld t0, UTF_X(5)(a0)
    ld t1, UTF_X(6)(a0)
    ld t2, UTF_X(7)(a0)
    ld s0, UTF_X(8)(a0)
    ld s1, UTF_X(9)(a0)
    ld a1, UTF_X(11)(a0)
    ld a2, UTF_X(12)(a0)
    ld a3, UTF_X(13)(a0)
    ld a4, UTF_X(14)(a0)
    ld a5, UTF_X(15)(a0)
    ld a6, UTF_X(16)(a0)
    ld a7, UTF_X(17)(a0)
    ld s2, UTF_X(18)(a0)
    ld s3, UTF_X(19)(a0)
    ld s4, UTF_X(20)(a0)
    ld s5, UTF_X(21)(a0)
    ld s6, UTF_X(22)(a0)
    ld s7, UTF_X(23)(a0)
    ld s8, UTF_X(24)(a0)
    ld s9, UTF_X(25)(a0)
    ld s10, UTF_X(26)(a0)
    ld s11, UTF_X(27)(a0)
    ld t3, UTF_X(28)(a0)
    ld t4, UTF_X(29)(a0)
    ld t5, UTF_X(30)(a0)
    ld t6, UTF_X(31)(a0)
    ld a0, UTF_X(10)(a0)
    sret

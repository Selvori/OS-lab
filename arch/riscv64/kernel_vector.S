#include <asm/trap.h>

.section .text
.balign 4
.globl kernel_vector
.type kernel_vector, @function
kernel_vector:
    # 硬件已记录 sepc/scause/sstatus 并清除 SIE，但不会保存通用寄存器。
    # 本阶段只有 S-mode 内核，沿用当前内核栈。288 是 16 的倍数。
    addi sp, sp, -TF_SIZE
    sd zero, TF_X(0)(sp)
    sd ra, TF_X(1)(sp)
    # t0 先保存，才能用它计算陷入前的 sp。
    sd t0, TF_X(5)(sp)
    addi t0, sp, TF_SIZE
    sd t0, TF_X(2)(sp)
    sd gp, TF_X(3)(sp)
    sd tp, TF_X(4)(sp)
    sd t1, TF_X(6)(sp)
    sd t2, TF_X(7)(sp)
    sd s0, TF_X(8)(sp)
    sd s1, TF_X(9)(sp)
    sd a0, TF_X(10)(sp)
    sd a1, TF_X(11)(sp)
    sd a2, TF_X(12)(sp)
    sd a3, TF_X(13)(sp)
    sd a4, TF_X(14)(sp)
    sd a5, TF_X(15)(sp)
    sd a6, TF_X(16)(sp)
    sd a7, TF_X(17)(sp)
    sd s2, TF_X(18)(sp)
    sd s3, TF_X(19)(sp)
    sd s4, TF_X(20)(sp)
    sd s5, TF_X(21)(sp)
    sd s6, TF_X(22)(sp)
    sd s7, TF_X(23)(sp)
    sd s8, TF_X(24)(sp)
    sd s9, TF_X(25)(sp)
    sd s10, TF_X(26)(sp)
    sd s11, TF_X(27)(sp)
    sd t3, TF_X(28)(sp)
    sd t4, TF_X(29)(sp)
    sd t5, TF_X(30)(sp)
    sd t6, TF_X(31)(sp)

    csrr t0, sstatus
    sd t0, TF_SSTATUS(sp)
    csrr t0, sepc
    sd t0, TF_SEPC(sp)
    csrr t0, scause
    sd t0, TF_SCAUSE(sp)
    csrr t0, stval
    sd t0, TF_STVAL(sp)

    # RISC-V C ABI：a0 是第一个参数。原 a0 已经保存在现场中。
    mv a0, sp
    call trap_kernel_handler

    # 保持恢复期间 SIE=0；sret 根据 SPIE 恢复陷入前的中断开关。
    ld t0, TF_SSTATUS(sp)
    andi t0, t0, -3
    csrw sstatus, t0
    ld t0, TF_SEPC(sp)
    csrw sepc, t0

    ld ra, TF_X(1)(sp)
    ld gp, TF_X(3)(sp)
    ld tp, TF_X(4)(sp)
    ld t0, TF_X(5)(sp)
    ld t1, TF_X(6)(sp)
    ld t2, TF_X(7)(sp)
    ld s0, TF_X(8)(sp)
    ld s1, TF_X(9)(sp)
    ld a0, TF_X(10)(sp)
    ld a1, TF_X(11)(sp)
    ld a2, TF_X(12)(sp)
    ld a3, TF_X(13)(sp)
    ld a4, TF_X(14)(sp)
    ld a5, TF_X(15)(sp)
    ld a6, TF_X(16)(sp)
    ld a7, TF_X(17)(sp)
    ld s2, TF_X(18)(sp)
    ld s3, TF_X(19)(sp)
    ld s4, TF_X(20)(sp)
    ld s5, TF_X(21)(sp)
    ld s6, TF_X(22)(sp)
    ld s7, TF_X(23)(sp)
    ld s8, TF_X(24)(sp)
    ld s9, TF_X(25)(sp)
    ld s10, TF_X(26)(sp)
    ld s11, TF_X(27)(sp)
    ld t3, TF_X(28)(sp)
    ld t4, TF_X(29)(sp)
    ld t5, TF_X(30)(sp)
    ld t6, TF_X(31)(sp)
    # 最后恢复 sp；不能用 ret，返回位置在 sepc 中。
    ld sp, TF_X(2)(sp)
    sret
.size kernel_vector, .-kernel_vector

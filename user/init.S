#include <kernel/syscall.h>
.section .text
.globl _start
.option norelax
.macro expect reg, value
    li a0, \value
    bne \reg, a0, fail
.endm
.macro write text
    li a0, 1
    la a1, \text
    la a2, \text\()_end
    sub a2, a2, a1
    li a7, SYS_WRITE
    ecall
.endm
.macro badptr address
    li a0, 1
    li a1, \address
    li a2, 8
    li a7, SYS_WRITE
    ecall
    li a1, -14
    bne a0, a1, fail
.endm
_start:
    write hello
    bne a0, a2, fail
    addi sp, sp, -16
    li t0, 0x2000
    li t1, 0x12345678
    sd t1, 0(t0)
    sd t1, 0(sp)
    li ra, 0x111
    li gp, 0x333
    li tp, 0x444
    li t2, 0x777
    li s0, 4096
    li s1, 0x999
    li a1, 0xaaa
    li a2, 0xbbb
    li a3, 0xccc
    li a4, 0xddd
    li a5, 0xeee
    li a6, 0xfff
    li s2, 0x1212
    li s3, 0x1313
    li s4, 0x1414
    li s5, 0x1515
    li s6, 0x1616
    li s7, 0x1717
    li s8, 0x1818
    li s9, 0x1919
    li s10, 0x2020
    li s11, 0x2121
    li t3, 0x2828
    li t4, 0x2929
    li t5, 0x3030
    li t6, 0x3131
round_trip:
    li a7, SYS_GETPID
    ecall
    addi a0, a0, -1
    bnez a0, fail
    expect ra, 0x111
    expect gp, 0x333
    expect tp, 0x444
    expect t0, 0x2000
    expect t1, 0x12345678
    expect t2, 0x777
    expect s1, 0x999
    expect a1, 0xaaa
    expect a2, 0xbbb
    expect a3, 0xccc
    expect a4, 0xddd
    expect a5, 0xeee
    expect a6, 0xfff
    expect a7, SYS_GETPID
    expect s2, 0x1212
    expect s3, 0x1313
    expect s4, 0x1414
    expect s5, 0x1515
    expect s6, 0x1616
    expect s7, 0x1717
    expect s8, 0x1818
    expect s9, 0x1919
    expect s10, 0x2020
    expect s11, 0x2121
    expect t3, 0x2828
    expect t4, 0x2929
    expect t5, 0x3030
    expect t6, 0x3131
    ld a0, 0(t0)
    bne a0, t1, fail
    ld a0, 0(sp)
    bne a0, t1, fail
    addi s0, s0, -1
    bnez s0, round_trip
    # The kernel returns errors without dereferencing or partially writing these buffers.
    li a7, 999
    ecall
    li a1, -38
    bne a0, a1, fail
    badptr 0
    badptr 0x80200000
    badptr 0x3fffffe000
    badptr 0x3fffffdffc
    badptr 0x3fffffc000
    badptr 0x3ffffff000
    badptr -4
    li a0, 1
    li a1, 0
    li a2, 257
    li a7, SYS_WRITE
    ecall
    li a1, -22
    bne a0, a1, fail
    li a0, 2
    li a2, 0
    li a7, SYS_WRITE
    ecall
    li a1, -9
    bne a0, a1, fail
    write waiting
    li a7, SYS_TICKS
    ecall
    mv s2, a0
wait_ticks:
    li a7, SYS_TICKS
    ecall
    sub t0, a0, s2
    li t1, 100
    bltu t0, t1, wait_ticks
    write passed
    li a0, 0
    li a7, SYS_EXIT
    ecall
fail:
    write failed
    li a0, 1
    li a7, SYS_EXIT
    ecall
1:  j 1b
hello: .ascii "Hello from Seele U-mode, pid 1!\n"
hello_end:
waiting: .ascii "USER WAIT: timer and UART active\n"
waiting_end:
passed: .ascii "USER PASS: 4096 round trips, registers, stack, data, errors, timer\n"
passed_end:
failed: .ascii "USER FAIL\n"
failed_end:

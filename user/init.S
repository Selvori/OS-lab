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
    call heap_demo
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
    call process_demo
    write lab6_pass
init_idle:
    li a0, 100
    li a7, SYS_SLEEP
    ecall
    j init_idle
fail:
    write failed
    li a0, 1
    li a7, SYS_EXIT
    ecall
1:  j 1b
hello: .ascii "Hello from Seele U-mode, pid 0!\n"
hello_end:
waiting: .ascii "USER WAIT: timer and UART active\n"
waiting_end:
passed: .ascii "USER PASS: 4096 round trips, registers, stack, data, errors, timer\n"
passed_end:
failed: .ascii "USER FAIL\n"
failed_end:

# New calls use ordinary register arguments; all memory below is U-mode memory.
heap_demo:
    addi sp, sp, -16
    sd ra, 0(sp)
    li a0, 0
    li a1, 3
    li a7, SYS_MMAP
    ecall
    li t0, 0x3000
    bne a0, t0, fail
    mv s0, a0
    mv t0, a0
    li t1, 12288
heap_zero:
    lbu t2, 0(t0)
    bnez t2, fail
    addi t0, t0, 1
    addi t1, t1, -1
    bnez t1, heap_zero
    li t0, 0x12345678
    sd t0, 0(s0)
    li t1, 12280
    add t1, t1, s0
    sd t0, 0(t1)
    # Copy a message across two independently backed virtual pages.
    li s1, 0x3ff8
    mv t0, s1
    la t1, heap_message
    la t2, heap_message_end
heap_copy:
    lbu t3, 0(t1)
    sb t3, 0(t0)
    addi t0, t0, 1
    addi t1, t1, 1
    bne t1, t2, heap_copy
    li a0, 1
    mv a1, s1
    la a2, heap_message_end
    la t0, heap_message
    sub a2, a2, t0
    li a7, SYS_WRITE
    ecall
    bne a0, a2, fail
    # A middle free splits live pages; first-fit must reuse this exact hole.
    li a0, 0x4000
    li a1, 1
    li a7, SYS_MUNMAP
    ecall
    bnez a0, fail
    li a0, 0
    li a1, 1
    li a7, SYS_MMAP
    ecall
    li t0, 0x4000
    bne a0, t0, fail
    ld t0, 0(a0)
    bnez t0, fail
    ld t0, 0(s0)
    li t1, 0x12345678
    bne t0, t1, fail
    li a0, 0x3000
    li a1, 1
    li a7, SYS_MMAP
    ecall
    li t0, -12
    bne a0, t0, fail
    # Overflow, zero pages, invalid alignment and nonheap frees are rejected.
    li a0, 0
    li a1, 0
    li a7, SYS_MMAP
    ecall
    li t0, -22
    bne a0, t0, fail
    li a0, -4096
    li a1, -1
    li a7, SYS_MMAP
    ecall
    li t0, -22
    bne a0, t0, fail
    li a0, 0x3001
    li a1, 1
    li a7, SYS_MUNMAP
    ecall
    li t0, -22
    bne a0, t0, fail
    li a0, 0x1000
    li a1, 1
    li a7, SYS_MUNMAP
    ecall
    li t0, -22
    bne a0, t0, fail
    li a0, 0x3000
    li a1, 3
    li a7, SYS_MUNMAP
    ecall
    bnez a0, fail
    li a0, 0x3000
    li a1, 1
    li a7, SYS_MUNMAP
    ecall
    li t0, -22
    bne a0, t0, fail
    # Explicit address exercises middle allocation, then bridging free.
    li a0, 0x7000
    li a1, 2
    li a7, SYS_MMAP
    ecall
    li t0, 0x7000
    bne a0, t0, fail
    li a0, 0
    li a1, 4
    li a7, SYS_MMAP
    ecall
    li t0, 0x3000
    bne a0, t0, fail
    li a0, 0x3000
    li a1, 4
    li a7, SYS_MUNMAP
    ecall
    bnez a0, fail
    li a0, 0x7000
    li a1, 2
    li a7, SYS_MUNMAP
    ecall
    bnez a0, fail
    li s2, 256
heap_repeat:
    li a0, 0
    li a1, 2
    li a7, SYS_MMAP
    ecall
    li t0, 0x3000
    bne a0, t0, fail
    ld t0, 0(a0)
    bnez t0, fail
    sd s2, 0(a0)
    li a1, 2
    li a7, SYS_MUNMAP
    ecall
    bnez a0, fail
    addi s2, s2, -1
    bnez s2, heap_repeat
    # Leave live mappings deliberately: exit must reclaim them too.
    li a0, 0
    li a1, 3
    li a7, SYS_MMAP
    ecall
    li t0, 0x3000
    bne a0, t0, fail
    write heap_pass
    ld ra, 0(sp)
    addi sp, sp, 16
    ret
heap_message: .ascii "HEAP cross-page write OK\n"
heap_message_end:
heap_pass: .ascii "HEAP PASS: zero, split, merge, reuse, errors, 256 cycles\n"
heap_pass_end:

# All descendants are ordinary fork copies of this code and private memory.
process_demo:
    addi sp, sp, -32
    sd ra, 24(sp)
    li t0, 0x4567
    sd t0, 8(sp)
    li t1, 0x3000
    sd t0, 0(t1)
    li a7, SYS_FORK
    ecall
    bltz a0, fail
    beqz a0, isolated_child
    mv s0, a0
    # Status pointer must be writable, including across a page boundary.
    li a0, 0x1ffe
    li a7, SYS_WAIT
    ecall
    li t0, -14
    bne a0, t0, fail
    mv a0, sp
    li a7, SYS_WAIT
    ecall
    bne a0, s0, fail
    lw t0, 0(sp)
    li t1, 37
    bne t0, t1, fail
    ld t0, 8(sp)
    li t1, 0x4567
    bne t0, t1, fail
    li t2, 0x3000
    ld t0, 0(t2)
    bne t0, t1, fail
    li t2, 0x2000
    ld t0, 0(t2)
    li t1, 0x12345678
    bne t0, t1, fail
    li t2, 0x4000
    sd t1, 0(t2)
    li a0, 0
    li a7, SYS_WAIT
    ecall
    li t0, -10
    bne a0, t0, fail
    li a0, -1
    li a7, SYS_SLEEP
    ecall
    li t0, -22
    bne a0, t0, fail
    li a0, 0
    li a7, SYS_EXIT
    ecall
    li t0, -1
    bne a0, t0, fail
    write fork_pass
    # No runnable process during this sleep: owner scheduler must idle/wake.
    li a7, SYS_TICKS
    ecall
    mv s1, a0
    li a0, 5
    li a7, SYS_SLEEP
    ecall
    bnez a0, fail
    li a7, SYS_TICKS
    ecall
    sub t0, a0, s1
    li t1, 5
    bltu t0, t1, fail
    write sleep_pass
    li a7, SYS_FORK
    ecall
    bltz a0, fail
    beqz a0, cpu_child
    mv s0, a0
    li a0, 3
    li a7, SYS_SLEEP
    ecall
    write parent_progress
    mv a0, sp
    li a7, SYS_WAIT
    ecall
    bne a0, s0, fail
    lw t0, 0(sp)
    li t1, 19
    bne t0, t1, fail
    # Repeated fork/wait exercises slot reuse and leaves heap to exit cleanup.
    li s2, 32
lifecycle:
    li a7, SYS_FORK
    ecall
    bltz a0, fail
    beqz a0, short_child
    mv s0, a0
    mv a0, sp
    li a7, SYS_WAIT
    ecall
    bne a0, s0, fail
    lw t0, 0(sp)
    bne t0, s2, fail
    addi s2, s2, -1
    bnez s2, lifecycle
    write lifecycle_pass
    ld ra, 24(sp)
    addi sp, sp, 32
    ret
isolated_child:
    ld t0, 8(sp)
    li t1, 0x4567
    bne t0, t1, fail
    li t2, 0x3000
    ld t0, 0(t2)
    bne t0, t1, fail
    sd zero, 0(t2)
    sd zero, 8(sp)
    li t2, 0x2000
    sd zero, 0(t2)
    li a0, 0x4000
    li a1, 1
    li a7, SYS_MUNMAP
    ecall
    bnez a0, fail
    li a0, 0
    li a1, 1
    li a7, SYS_MMAP
    ecall
    li t0, 0x4000
    bne a0, t0, fail
    li a0, 3
    li a7, SYS_SLEEP
    ecall
    li a0, 37
    li a7, SYS_EXIT
    ecall
    j fail
cpu_child:
    write cpu_begin
    li gp, 0x333
    li tp, 0x444
    li t0, 200000000
    li t1, 0
    li t2, 7
cpu_loop:
    add t1, t1, t2
    addi t0, t0, -1
    bnez t0, cpu_loop
    li t0, 1400000000
    bne t1, t0, fail
    li t0, 0x333
    bne gp, t0, fail
    li t0, 0x444
    bne tp, t0, fail
    write cpu_end
    li a0, 19
    li a7, SYS_EXIT
    ecall
    j fail
short_child:
    mv a0, s2
    li a7, SYS_EXIT
    ecall
    j fail
fork_pass: .ascii "FORK/WAIT PASS: independent data, stack, heap, status, errors\n"
fork_pass_end:
sleep_pass: .ascii "SLEEP PASS: deadline and idle wakeup\n"
sleep_pass_end:
cpu_begin: .ascii "CPU child: begin loop without syscalls\n"
cpu_begin_end:
parent_progress: .ascii "PREEMPT PASS: parent ran while CPU child busy\n"
parent_progress_end:
cpu_end: .ascii "CPU child: registers and loop result preserved\n"
cpu_end_end:
lifecycle_pass: .ascii "LIFECYCLE PASS: 32 fork/wait cycles\n"
lifecycle_pass_end:
lab6_pass: .ascii "LAB6 PASS: init pid 0 remains alive\n"
lab6_pass_end:

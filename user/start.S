#include <kernel/syscall.h>
.section .text.start,"ax",@progbits
.globl _start
_start:
    call main
    li a7, SYS_EXIT
    ecall
1:  li a0, 100
    li a7, SYS_SLEEP
    ecall
    j 1b

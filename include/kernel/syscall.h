#ifndef SEELE_SYSCALL_H
#define SEELE_SYSCALL_H
#define SYS_WRITE 1
#define SYS_GETPID 2
#define SYS_TICKS 3
#define SYS_EXIT 4
#define SYS_WRITE_MAX 256
#ifndef __ASSEMBLER__
#include <kernel/proc.h>
void syscall_dispatch(struct proc *p);
#endif
#endif

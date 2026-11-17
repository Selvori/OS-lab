#include <kernel/syscall.h>
#include <kernel/proc.h>
#include <kernel/timer.h>

void syscall_dispatch(struct proc *p)
{
    struct user_trapframe *tf = p->tf;
    long result;
    p->syscalls++;
    switch (tf->x[17]) {
    case SYS_MMAP:
        result = user_heap_alloc(p, tf->x[10], tf->x[11]);
        break;
    case SYS_MUNMAP:
        result = user_heap_free(p, tf->x[10], tf->x[11]);
        break;
    case SYS_GETPID:
        result = p->pid;
        break;
    case SYS_TICKS:
        result = (long)timer_ticks(p->owner);
        break;
    case SYS_FORK:
        result = proc_fork();
        break;
    case SYS_WAIT:
        result = proc_wait(tf->x[10]);
        break;
    case SYS_SLEEP:
        result = proc_sleep_ticks(tf->x[10]);
        break;
    case SYS_YIELD:
        proc_yield();
        result = 0;
        break;
    case SYS_EXIT:
        result = proc_finish((int)tf->x[10], false);
        break;
    default:
        result = file_syscall(p);
        break;
    }
    tf->x[10] = (uint64)result;
}

#include <kernel/syscall.h>
#include <kernel/print.h>
#include <kernel/timer.h>

void syscall_dispatch(struct proc *p)
{
    struct user_trapframe *tf = p->tf;
    long result;
    p->syscalls++;
    switch (tf->x[17]) {
    case SYS_WRITE: {
        char buffer[SYS_WRITE_MAX];
        size_t count = tf->x[12];
        if (tf->x[10] != 1) { result = -9; break; }
        if (count > sizeof(buffer)) { result = -22; break; }
        if (user_copyin(p->pgtbl, buffer, tf->x[11], count) != 0) {
            result = -14;
            break;
        }
        for (size_t i = 0; i < count; ++i)
            printf("%c", buffer[i]);
        result = (long)count;
        break;
    }
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
    case SYS_EXIT:
        proc_finish((int)tf->x[10], false);
    default:
        result = -38;
        break;
    }
    tf->x[10] = (uint64)result;
}

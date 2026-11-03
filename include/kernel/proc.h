#ifndef SEELE_PROC_H
#define SEELE_PROC_H
#include <asm/usertrap.h>
#include <kernel/heap.h>
#define NPROC 16
struct context { uint64 ra, sp, s[12]; };
_Static_assert(offsetof(struct context, s) == 16, "switch offsets");
_Static_assert(sizeof(struct context) == 112, "switch size");
enum proc_state { PROC_UNUSED, PROC_USED, PROC_RUNNABLE, PROC_RUNNING,
                  PROC_SLEEPING, PROC_ZOMBIE };
struct proc {
    int pid;
    pagetable_t pgtbl;
    uintptr_t heap_top;
    mmap_region_t *mmap;
    unsigned ustack_pages;
    struct user_trapframe *tf;
    uintptr_t kstack;
    struct context ctx;
    void *code, *data, *ustack, *stack_page;
    enum proc_state state;
    uint64 owner, user_timers, user_external, syscalls, switches;
    struct proc *parent;
    void *chan;
    uint64 deadline;
    int status;
    bool stack_mapped, faulted;
};
void proc_init(void);
int proc_make_first(void);
void proc_scheduler(void) __attribute__((noreturn));
struct proc *proc_current(void);
long proc_fork(void);
long proc_wait(uintptr_t status_address);
int proc_sleep_ticks(uint64 duration);
void proc_yield(void);
void proc_sleep(void *chan);
void proc_wakeup(void *chan);
void proc_tick(void);
int proc_finish(int status, bool fault);
void proc_destroy(struct proc *p);
void swtch(struct context *old, struct context *new);
#endif

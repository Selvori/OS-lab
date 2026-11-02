#ifndef SEELE_PROC_H
#define SEELE_PROC_H
#include <asm/usertrap.h>
#include <kernel/user.h>
struct context {
    uint64 ra, sp, s[12];
};
_Static_assert(offsetof(struct context, s) == 16, "switch offsets");
_Static_assert(sizeof(struct context) == 112, "switch size");
enum proc_state { PROC_EMPTY, PROC_READY, PROC_RUNNING, PROC_EXITED, PROC_FAULTED };
struct proc {
    int pid;
    pagetable_t pgtbl;
    uintptr_t heap_top;
    unsigned ustack_pages;
    struct user_trapframe *tf;
    uintptr_t kstack;
    struct context ctx;
    void *code, *data, *ustack, *stack_page;
    enum proc_state state;
    uint64 owner, user_timers, user_external, syscalls;
    int status;
    bool stack_mapped;
};
int proc_make_first(void);
void proc_run(void);
struct proc *proc_current(void);
void proc_finish(int status, bool fault) __attribute__((noreturn));
void proc_destroy(void);
void swtch(struct context *old, struct context *new);
#endif

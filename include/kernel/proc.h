#ifndef SEELE_PROC_H
#define SEELE_PROC_H
#include <asm/usertrap.h>
#include <kernel/heap.h>
#include <kernel/file.h>
#include <kernel/template.h>
struct kfile;
#define NPROC 16
struct context { uint64 ra, sp, s[12]; };
_Static_assert(offsetof(struct context, s) == 16, "switch offsets");
_Static_assert(sizeof(struct context) == 112, "switch size");
enum proc_state { PROC_UNUSED, PROC_USED, PROC_RUNNABLE, PROC_RUNNING,
                  PROC_SLEEPING, PROC_ZOMBIE, PROC_REMOTE };
struct proc {
    int pid;
    struct proc *template_handle;
    struct branch_heap *branch;
    struct template_job *reclaim_job;
    unsigned template_branches;
    bool template_ref_batch;
    unsigned template_refs;
    bool template_owned, template_reclaim, template_tracking, mixed_heap;
    uint16_t template_reclaimed;
    uint8 template_borrowers[HEAP_WINDOWS];
#ifdef LIFECYCLE_PROBE
    bool timed_heap_write;
#endif
    pagetable_t pgtbl;
    uintptr_t heap_top;
    uintptr_t cow_next;
    mmap_region_t *mmap;
    unsigned ustack_pages;
    struct user_trapframe *tf;
    uintptr_t kstack;
    struct context ctx;
    void *code, *data, *ustack, *stack_page;
    enum proc_state state;
    uint64 owner, user_timers, user_external, syscalls, switches;
    struct proc *parent;
    struct kfile *files[NFD];
    uint32 cwd;
    void *chan;
    uint64 deadline;
    int status;
    bool stack_mapped, faulted;
#ifdef DUAL_HART_USERS
    bool user_ready; /* Runnable with no saved kernel continuation. */
    bool template_parallel; /* Policy permits branch U-mode leases. */
    bool prefer_owner;
    uint64 timer_syscalls, remote_cause, remote_value;
#endif
    struct template_job *yield_reclaim;
};
void proc_init(void);
int proc_make_first(void);
void proc_scheduler(void) __attribute__((noreturn));
struct proc *proc_current(void);
long proc_fork(void);
long proc_wait(uintptr_t status_address);
int proc_sleep_ticks(uint64 duration);
void proc_yield(bool voluntary);
#ifdef DUAL_HART_USERS
void proc_preempt_user(bool timer) __attribute__((noreturn));
#endif
void proc_sleep(void *chan);
void proc_wakeup(void *chan);
void proc_tick(void);
int proc_finish(int status, bool fault);
void proc_destroy(struct proc *p);
void swtch(struct context *old, struct context *new);
#endif

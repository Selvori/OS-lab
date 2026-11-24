#include <kernel/lifecycle_probe.h>
#include <asm/csr.h>
#include <asm/user_worker.h>
#include <kernel/arch.h>
#include <kernel/proc.h>
#include <kernel/print.h>
#include <kernel/syscall.h>
#include <kernel/trap.h>

extern char trampoline[], user_vector[], user_return[];
extern void kernel_vector(void);

static void handle_user_trap(struct proc *p, uint64 cause, uint64 value)
    __attribute__((noreturn));

static void handle_user_trap(struct proc *p, uint64 cause, uint64 value)
{
    if (cause == 8) {
        p->tf->epc += 4; /* ECALL is a 32-bit instruction, including with RVC. */
        syscall_dispatch(p);
    } else if (cause == 15 && user_cow_fault(p, value) == 0) {
        /* Retry the original store at the saved PC. */
    } else if (cause & SCAUSE_INTERRUPT) {
        if (!trap_device_interrupt(cause))
            goto fault;
        if ((cause & ~SCAUSE_INTERRUPT) == IRQ_S_TIMER) {
            p->user_timers++;
#ifdef DUAL_HART_USERS
            proc_preempt_user(true);
#else
            proc_yield(false);
#endif
        }
#ifdef DUAL_HART_USERS
        if ((cause & ~SCAUSE_INTERRUPT) == IRQ_S_SOFT)
            proc_preempt_user(false);
#endif
        if ((cause & ~SCAUSE_INTERRUPT) == IRQ_S_EXT) p->user_external++;
    } else {
fault:
        printf("USER FAULT pid=%d scause=%p epc=%p stval=%p\n", p->pid,
               (void *)cause, (void *)p->tf->epc, (void *)value);
        proc_finish(-1, true);
    }
    trap_user_return();
}

void trap_user_handler(void)
{
    uint64 status = csr_read(sstatus);
    uint64 cause = csr_read(scause);
    uint64 value = csr_read(stval);
    struct proc *p = proc_current();
    csr_write(stvec, (uint64)kernel_vector);
    assert(p && !(status & SSTATUS_SPP) && !arch_irq_enabled(), "invalid user trap");
    p->tf->epc = csr_read(sepc);
    handle_user_trap(p, cause, value);
}

#ifdef DUAL_HART_USERS
void trap_user_resume_remote(void)
{
    struct proc *p = proc_current();
    assert(p && p->state == PROC_RUNNING && !arch_irq_enabled(),
           "invalid remote trap replay");
    handle_user_trap(p, p->remote_cause, p->remote_value);
}
#endif

void trap_user_enter(struct user_trapframe *tf, pagetable_t root, uintptr_t sp,
                     void (*handler)(void))
{
    tf->kernel_satp = csr_read(satp);
    tf->kernel_sp = sp;
    tf->kernel_trap = (uint64)handler;
    tf->kernel_hart = arch_hart_id();
    __asm__ volatile("mv %0, gp" : "=r"(tf->kernel_gp));
    csr_write(stvec, TRAMPOLINE + (uintptr_t)(user_vector - trampoline));
    uint64 status = csr_read(sstatus);
    /* No floating-point/vector context is saved; keep both disabled in U-mode. */
    status &= ~(SSTATUS_SPP | SSTATUS_SIE | (1UL << 18) | (1UL << 19) |
                (3UL << 13) | (3UL << 9));
    csr_write(sstatus, status | SSTATUS_SPIE);
    csr_write(sepc, tf->epc);
#ifdef LIFECYCLE_PROBE
    csr_write(scounteren, 1UL << 1); /* Expose time only in research builds. */
#endif
    uint64 satp = SATP_MODE_SV39 | ((uintptr_t)root >> PAGE_SHIFT);
    void (*ret)(uint64, uint64) = (void *)(TRAMPOLINE + (uintptr_t)(user_return - trampoline));
    ret(TRAPFRAME, satp);
    __builtin_unreachable();
}

void trap_user_return(void)
{
    arch_irq_disable();
    struct proc *p = proc_current();
    assert(p && p->state == PROC_RUNNING, "no running user process");
    trap_user_enter(p->tf, p->pgtbl, p->kstack + PAGE_SIZE, trap_user_handler);
}

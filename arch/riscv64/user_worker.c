#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/user_worker.h>
#include <kernel/arch.h>
#include <kernel/print.h>
#include <kernel/proc.h>
#include <kernel/trap.h>
#include <platform.h>

_Static_assert(NCPU == 2, "user worker requires exactly two harts");

enum worker_state { WORKER_IDLE, WORKER_READY, WORKER_DONE };

static struct {
    enum worker_state state;
    struct user_trapframe *tf;
    pagetable_t root;
    uint64 cause, value;
} job;
static struct context loop_context, user_context;
/* Avoid the recyclable per-process kernel stack aliases on the worker. */
static uint8 trap_stack[PAGE_SIZE] __attribute__((aligned(16)));
extern void kernel_vector(void);

static void notify_peer(void)
{
    /* Publish ordinary memory before requesting the peer's interrupt. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (sbi_send_ipi(HART_FIRST + (1 - arch_cpu_id())) != 0)
        panic("SBI IPI failed");
}

void user_worker_submit(struct user_trapframe *tf, pagetable_t root)
{
    assert(__atomic_load_n(&job.state, __ATOMIC_RELAXED) == WORKER_IDLE,
           "user worker already assigned");
    job.tf = tf;
    job.root = root;
    __atomic_store_n(&job.state, WORKER_READY, __ATOMIC_RELEASE);
    notify_peer();
}

bool user_worker_collect(uint64 *cause, uint64 *value)
{
    if (__atomic_load_n(&job.state, __ATOMIC_ACQUIRE) != WORKER_DONE)
        return false;
    *cause = job.cause;
    *value = job.value;
    __atomic_store_n(&job.state, WORKER_IDLE, __ATOMIC_RELAXED);
    return true;
}

static void worker_enter(void) __attribute__((noreturn));

static void worker_trap(void)
{
    uint64 status = csr_read(sstatus);
    uint64 cause = csr_read(scause);
    csr_write(stvec, (uint64)kernel_vector);
    assert(!(status & SSTATUS_SPP) && !arch_irq_enabled() &&
           kernel_vm_active(), "invalid worker user trap");
    job.tf->epc = csr_read(sepc);
    if (cause & SCAUSE_INTERRUPT) {
        assert(trap_device_interrupt(cause), "unexpected worker interrupt");
        /* Device service stays on this hart; only the local tick ends the slice. */
        if ((cause & ~SCAUSE_INTERRUPT) != IRQ_S_TIMER)
            worker_enter();
    }
    job.cause = cause;
    job.value = csr_read(stval);
    swtch(&user_context, &loop_context);
    panic("resumed discarded worker continuation");
}

static void worker_enter(void)
{
    trap_user_enter(job.tf, job.root, (uintptr_t)(trap_stack + sizeof(trap_stack)),
                    worker_trap);
}

void user_worker_loop(void)
{
    arch_irq_disable();
    for (;;) {
        if (__atomic_load_n(&job.state, __ATOMIC_ACQUIRE) == WORKER_READY) {
            user_context = (struct context) {
                .ra = (uintptr_t)worker_enter,
                .sp = (uintptr_t)(trap_stack + sizeof(trap_stack)),
            };
            /* The owner installed the code bytes before publishing this job. */
            __asm__ volatile("fence.i" ::: "memory");
            swtch(&loop_context, &user_context);
            /* The trampoline fenced after switching back to the kernel root.
             * Publish only from the permanent worker loop stack. */
            __atomic_store_n(&job.state, WORKER_DONE, __ATOMIC_RELEASE);
            notify_peer();
        } else {
            /* An SSIP arriving between the acquire and WFI also wakes WFI. */
            __asm__ volatile("wfi");
        }
        arch_irq_enable();
        arch_irq_disable();
    }
}

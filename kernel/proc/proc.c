#include <kernel/proc.h>
#include <kernel/arch.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <kernel/vm.h>
#include <asm/csr.h>
#include <platform.h>

/* Only scheduler_owner touches this table. Other harts service devices only. */
static struct proc procs[NPROC];
static struct proc *initproc;
static struct context scheduler_context;
static struct proc *current[NCPU];
static uint64 scheduler_owner, sched_ticks;
static unsigned next_pid;

static void owner_context(void)
{
    assert(arch_cpu_id() == scheduler_owner && !arch_irq_enabled(), "process owner context");
}

struct proc *proc_current(void) { return current[arch_cpu_id()]; }

void proc_init(void)
{
    assert(arch_is_boot_cpu() && !arch_irq_enabled(), "process init context");
    scheduler_owner = arch_cpu_id();
    next_pid = 1;
}

void proc_destroy(struct proc *p)
{
    owner_context();
    assert(p && p != proc_current() && p->state != PROC_RUNNING &&
           p->state != PROC_SLEEPING, "destroy active process");
    user_heap_destroy(p);
    if (p->stack_mapped) {
        pte_t *pte = sv39_walk(kernel_vm_root(), p->kstack, false);
        assert(pte && (*pte & PTE_V), "kernel stack leaf missing");
        *pte = 0;
        sfence_vma();
    }
    if (p->pgtbl) sv39_free_tables(p->pgtbl);
    if (p->tf) page_free(p->tf);
    if (p->stack_page) page_free(p->stack_page);
    if (p->code) page_free(p->code);
    if (p->data) page_free(p->data);
    if (p->ustack) page_free(p->ustack);
    memset(p, 0, sizeof(*p));
}

static struct proc *proc_allocate(void)
{
    owner_context();
    struct proc *p = NULL;
    for (unsigned i = 0; i < NPROC; ++i) {
        if (procs[i].state != PROC_UNUSED) continue;
        p = &procs[i];
        p->state = PROC_USED; /* Not visible to the scheduler yet. */
        p->owner = scheduler_owner;
        p->kstack = USER_KSTACK - 2 * PAGE_SIZE * i;
        break;
    }
    if (!p) return NULL;
    p->ustack_pages = 1;
    p->pgtbl = page_alloc(PAGE_ZONE_KERNEL);
    p->tf = page_alloc(PAGE_ZONE_KERNEL);
    p->stack_page = page_alloc(PAGE_ZONE_KERNEL);
    p->code = page_alloc(PAGE_ZONE_USER);
    p->data = page_alloc(PAGE_ZONE_USER);
    p->ustack = page_alloc(PAGE_ZONE_USER);
    if (!p->pgtbl || !p->tf || !p->stack_page || !p->code || !p->data || !p->ustack)
        goto fail;
    if (user_vm_create(p) != 0) goto fail;
    /* Trampoline already allocated this shared kernel table topology at boot. */
    if (sv39_map(kernel_vm_root(), p->kstack, (uintptr_t)p->stack_page,
                 PAGE_SIZE, PTE_R | PTE_W) != 0) goto fail;
    p->stack_mapped = true;
    sfence_vma();
    p->ctx.ra = (uint64)trap_user_return;
    p->ctx.sp = p->kstack + PAGE_SIZE;
    return p;
fail:
    proc_destroy(p);
    return NULL;
}

int proc_make_first(void)
{
    owner_context();
    assert(!initproc, "init already exists");
    struct proc *p = proc_allocate();
    if (!p) return -12;
    if (user_heap_init(p) != 0) { proc_destroy(p); return -12; }
    p->pid = 0;
    p->tf->epc = USER_BASE;
    p->tf->x[2] = TRAPFRAME;
    initproc = p;
    p->state = PROC_RUNNABLE;
    return 0;
}

static void copy_page(void *dst, const void *src)
{
    for (size_t i = 0; i < PAGE_SIZE; ++i) ((uint8 *)dst)[i] = ((const uint8 *)src)[i];
}

long proc_fork(void)
{
    owner_context();
    struct proc *parent = proc_current();
    assert(parent && parent->state == PROC_RUNNING, "fork current");
    bool slot = false;
    for (unsigned i = 0; i < NPROC; ++i) if (procs[i].state == PROC_UNUSED) slot = true;
    if (!slot || next_pid > 0x7fffffffU) return -11;
    struct proc *child = proc_allocate();
    if (!child) return -12;
    copy_page(child->code, parent->code);
    copy_page(child->data, parent->data);
    copy_page(child->ustack, parent->ustack);
    if (user_heap_clone(child, parent) != 0) { proc_destroy(child); return -12; }
    *child->tf = *parent->tf;
    child->tf->x[10] = 0;
    child->parent = parent;
    child->pid = (int)next_pid++;
    __asm__ volatile("fence.i" ::: "memory");
    child->state = PROC_RUNNABLE;
    return child->pid;
}

/* No interrupt nesting or spinlock may span a context switch. */
static void proc_sched(void)
{
    owner_context();
    struct proc *p = proc_current();
    assert(p && p->state != PROC_RUNNING && arch_irq_depth() == 0, "sched context");
    swtch(&p->ctx, &scheduler_context);
    assert(proc_current() == p && p->state == PROC_RUNNING && !arch_irq_enabled(),
           "scheduler resumed wrong process");
}

void proc_yield(void)
{
    owner_context();
    struct proc *p = proc_current();
    assert(p && p->state == PROC_RUNNING, "yield current");
    p->state = PROC_RUNNABLE;
    proc_sched();
}

void proc_sleep(void *chan)
{
    owner_context();
    struct proc *p = proc_current();
    assert(p && p->state == PROC_RUNNING && chan, "sleep current");
    p->chan = chan;
    p->state = PROC_SLEEPING;
    proc_sched();
    p->chan = NULL;
}

void proc_wakeup(void *chan)
{
    owner_context();
    for (unsigned i = 0; i < NPROC; ++i)
        if (procs[i].state == PROC_SLEEPING && procs[i].chan == chan)
            procs[i].state = PROC_RUNNABLE;
}

int proc_sleep_ticks(uint64 duration)
{
    owner_context();
    if (duration > 0x7fffffffUL) return -22;
    if (!duration) { proc_yield(); return 0; }
    struct proc *p = proc_current();
    p->deadline = sched_ticks + duration;
    while ((int64_t)(sched_ticks - p->deadline) < 0) proc_sleep(&sched_ticks);
    return 0;
}

void proc_tick(void)
{
    if (arch_cpu_id() != scheduler_owner) return;
    owner_context();
    ++sched_ticks;
    for (unsigned i = 0; i < NPROC; ++i) {
        struct proc *p = &procs[i];
        if (p->state == PROC_SLEEPING && p->chan == &sched_ticks &&
            (int64_t)(sched_ticks - p->deadline) >= 0) p->state = PROC_RUNNABLE;
    }
}

long proc_wait(uintptr_t address)
{
    owner_context();
    struct proc *p = proc_current();
    if (address && !user_writable(p->pgtbl, address, sizeof(int))) return -14;
    for (;;) {
        bool children = false;
        for (unsigned i = 0; i < NPROC; ++i) {
            struct proc *child = &procs[i];
            if (child->state == PROC_UNUSED || child->parent != p) continue;
            children = true;
            if (child->state != PROC_ZOMBIE) continue;
            int pid = child->pid;
            if (address && user_copyout(p->pgtbl, address, &child->status, sizeof(int)) != 0)
                return -14;
            proc_destroy(child);
            return pid;
        }
        if (!children) return -10;
        /* Check and publication are indivisible: only owner modifies the table,
         * and owner IRQ is off until after switching to scheduler context. */
        proc_sleep(p);
    }
}

int proc_finish(int status, bool fault)
{
    owner_context();
    struct proc *p = proc_current();
    assert(p && p->state == PROC_RUNNING, "exit current");
    if (p == initproc) {
        if (fault) panic("init user fault");
        return -1;
    }
    for (unsigned i = 0; i < NPROC; ++i) {
        if (procs[i].state == PROC_UNUSED || procs[i].parent != p) continue;
        procs[i].parent = initproc;
        if (procs[i].state == PROC_ZOMBIE) proc_wakeup(initproc);
    }
    p->status = status;
    p->faulted = fault;
    p->state = PROC_ZOMBIE;
    proc_wakeup(p->parent);
    proc_sched();
    panic("zombie resumed");
}

void proc_scheduler(void)
{
    owner_context();
    assert(initproc && !proc_current(), "scheduler init");
    for (;;) {
        bool ran = false;
        for (unsigned i = 0; i < NPROC; ++i) {
            struct proc *p = &procs[i];
            if (p->state != PROC_RUNNABLE) continue;
            ran = true;
            current[scheduler_owner] = p;
            p->state = PROC_RUNNING;
            ++p->switches;
            swtch(&scheduler_context, &p->ctx);
            assert(!arch_irq_enabled() && p->state != PROC_RUNNING, "scheduler return");
            current[scheduler_owner] = NULL;
        }
        if (!ran) {
            /* WFI wakes on a locally enabled pending timer even with SIE=0.
             * Deliver it on the boot stack before checking runnable state. */
            __asm__ volatile("wfi");
        }
        arch_irq_enable();
        arch_irq_disable();
    }
}

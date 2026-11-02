#include <kernel/proc.h>
#include <kernel/arch.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <kernel/vm.h>
#include <asm/csr.h>
#include <platform.h>

static struct proc first;
static struct context boot_context;
static struct proc *current[NCPU];

struct proc *proc_current(void)
{
    return current[arch_cpu_id()];
}

void proc_destroy(void)
{
    struct proc *p = &first;
    assert(!arch_irq_enabled() && proc_current() == NULL, "destroy active process");
    user_heap_destroy(p);
    if (p->stack_mapped) {
        pte_t *pte = sv39_walk(kernel_vm_root(), p->kstack, false);
        assert(pte != NULL, "kernel stack leaf missing");
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

int proc_make_first(void)
{
    struct proc *p = &first;
    assert(arch_is_boot_cpu() && !arch_irq_enabled(), "process create context");
    assert(p->state == PROC_EMPTY, "process already exists");
    p->pid = 1;
    p->owner = arch_cpu_id();
    p->kstack = USER_KSTACK;
    p->heap_top = 0;
    p->ustack_pages = 1;
    p->pgtbl = page_alloc(PAGE_ZONE_KERNEL);
    p->tf = page_alloc(PAGE_ZONE_KERNEL);
    p->stack_page = page_alloc(PAGE_ZONE_KERNEL);
    p->code = page_alloc(PAGE_ZONE_USER);
    p->data = page_alloc(PAGE_ZONE_USER);
    p->ustack = page_alloc(PAGE_ZONE_USER);
    if (!p->pgtbl || !p->tf || !p->stack_page || !p->code || !p->data || !p->ustack)
        goto fail;
    if (user_vm_create(p) != 0)
        goto fail;
    if (user_heap_init(p) != 0)
        goto fail;
    /* The trampoline mapping already created these shared kernel table nodes.
     * Secondary harts never use this stack alias or modify its leaf. */
    if (sv39_map(kernel_vm_root(), p->kstack, (uintptr_t)p->stack_page,
                 PAGE_SIZE, PTE_R | PTE_W) != 0)
        goto fail;
    p->stack_mapped = true;
    sfence_vma();
    p->tf->epc = USER_BASE;
    p->tf->x[2] = TRAPFRAME;
    p->ctx.ra = (uint64)trap_user_return;
    p->ctx.sp = p->kstack + PAGE_SIZE;
    p->state = PROC_READY;
    return 0;
fail:
    proc_destroy();
    return -1;
}

void proc_run(void)
{
    struct proc *p = &first;
    arch_irq_disable();
    assert(p->state == PROC_READY && p->owner == arch_cpu_id(), "process run owner");
    p->state = PROC_RUNNING;
    current[p->owner] = p;
    printf("process %d entering U-mode: entry=%p stack=%p\n",
           p->pid, (void *)p->tf->epc, (void *)p->tf->x[2]);
    swtch(&boot_context, &p->ctx);
    current[p->owner] = NULL;
    printf("process %d %s: status=%d syscalls=%d user-timer=%d user-external=%d\n",
           p->pid, p->state == PROC_EXITED ? "exited" : "faulted",
           p->status, (int)p->syscalls, (int)p->user_timers, (int)p->user_external);
}

void proc_finish(int status, bool fault)
{
    struct proc *p = proc_current();
    assert(p && p->owner == arch_cpu_id() && !arch_irq_enabled(), "process finish state");
    p->status = status;
    p->state = fault ? PROC_FAULTED : PROC_EXITED;
    swtch(&p->ctx, &boot_context);
    panic("terminated process resumed");
}

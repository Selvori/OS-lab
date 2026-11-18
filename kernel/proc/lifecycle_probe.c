#include <kernel/lifecycle_probe.h>
#include <kernel/arch.h>
#include <kernel/proc.h>
#include <kernel/print.h>
#include <kernel/string.h>

struct lifecycle_sample lifecycle_sample;
bool lifecycle_active;

static struct lifecycle_heap_detail heap_detail;

#ifdef LIFECYCLE_COUNTS
bool lifecycle_in_heap;
unsigned lifecycle_heap_phase;

void lifecycle_heap_check(uintptr_t va, unsigned level)
{
    if (!lifecycle_active || !lifecycle_in_heap)
        return;
    if (level != 0) {
        ++lifecycle_sample.upper_checks;
        return;
    }
    unsigned table = sv39_index(1, va);
    assert(table < PROBE_HEAP_TABLES, "heap probe range");
    ++heap_detail.checks[lifecycle_heap_phase][table];
    ++lifecycle_sample.leaf_checks;
}

void lifecycle_page(bool user, bool allocated)
{
    if (!lifecycle_active)
        return;
    struct lifecycle_sample *s = &lifecycle_sample;
    unsigned long *alloc = user ? &s->user_alloc : &s->kernel_alloc;
    unsigned long *freed = user ? &s->user_free : &s->kernel_free;
    unsigned long *peak = user ? &s->user_peak : &s->kernel_peak;
    if (allocated) {
        ++*alloc;
        if (*alloc > *freed && *alloc - *freed > *peak)
            *peak = *alloc - *freed;
    } else {
        ++*freed;
    }
}

#endif

#ifdef LIFECYCLE_CHECKS
static int fail_pid = -1;
static unsigned long remaining;

bool lifecycle_alloc_fails(void)
{
    if (!arch_is_boot_cpu() || fail_pid < 0)
        return false;
    struct proc *p = proc_current();
    if (!p || p->pid != fail_pid)
        return false;
    if (remaining) {
        --remaining;
        return false;
    }
    fail_pid = -1;
    return true;
}

static long inspect_mapping(struct proc *p, uintptr_t address)
{
    struct lifecycle_mapping mapping;
    if (user_copyin(p->pgtbl, &mapping.address, address, sizeof(mapping.address)) != 0 ||
        !sv39_lookup(p->pgtbl, mapping.address, &mapping.frame, &mapping.flags) ||
        !(mapping.flags & PTE_U))
        return -14;
    mapping.frame = PAGE_ROUND_DOWN(mapping.frame);
    mapping.refs = page_user_refs((void *)mapping.frame);
    return user_copyout(p, address, &mapping, sizeof(mapping));
}
#endif

long lifecycle_control(struct proc *p, unsigned long op, uintptr_t address)
{
#ifdef LIFECYCLE_CHECKS
    if (op == PROBE_FAIL_AFTER) {
        fail_pid = p->pid;
        remaining = address;
        return 0;
    }
    if (op == PROBE_MAPPING)
        return inspect_mapping(p, address);
#endif
    if (op == PROBE_HEAP_DETAIL) {
        if (lifecycle_active)
            return -22;
        return user_copyout(p, address, &heap_detail, sizeof(heap_detail));
    }
    if (op == PROBE_START) {
        memset(&lifecycle_sample, 0, sizeof(lifecycle_sample));
        memset(&heap_detail, 0, sizeof(heap_detail));
        lifecycle_active = true;
        return 0;
    }
    if (op != PROBE_STOP && op != PROBE_SNAPSHOT)
        return -22;
    lifecycle_active = false;
    struct lifecycle_sample sample = {0};
    if (op == PROBE_STOP)
        sample = lifecycle_sample;
    sample.user_left = page_free_count(PAGE_ZONE_USER);
    sample.kernel_left = page_free_count(PAGE_ZONE_KERNEL);
    sample.nodes_left = mmap_pool_free_count();
    sample.slots_used = proc_slots_used();
    sample.files_used = files_used();
    return user_copyout(p, address, &sample, sizeof(sample));
}

#include <kernel/lifecycle_probe.h>
#include <kernel/proc.h>
#include <kernel/string.h>

struct lifecycle_sample lifecycle_sample;
bool lifecycle_active;

#ifdef LIFECYCLE_COUNTS
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

long lifecycle_control(struct proc *p, unsigned long op, uintptr_t address)
{
    if (op == PROBE_START) {
        memset(&lifecycle_sample, 0, sizeof(lifecycle_sample));
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
    return user_copyout(p->pgtbl, address, &sample, sizeof(sample)) == 0 ? 0 : -14;
}

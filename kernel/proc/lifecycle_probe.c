#include <kernel/lifecycle_probe.h>
#include <kernel/arch.h>
#include <kernel/proc.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <platform.h>

struct lifecycle_sample lifecycle_sample;
bool lifecycle_active;

struct template_stats template_stats;
static struct lifecycle_heap_detail heap_detail;
struct reclaim_stats reclaim_stats;

/* Instrumented builds reserve a reverse map so the page allocator can mark
 * the first return of each watched PA even after the template drops it. */
#define TRACE_CAPACITY 8192
#define WATCH_CAPACITY USER_HEAP_PAGES
static uint16_t frame_index[DRAM_SIZE / PAGE_SIZE];
static struct reclaim_frame original_frames[WATCH_CAPACITY];
static unsigned watched, trace_count;
static struct reclaim_trace trace[TRACE_CAPACITY];

bool lifecycle_reclaim_recording(void)
{
    return lifecycle_active && watched;
}

void lifecycle_reclaim_trace(uint64 ticks, unsigned confirmed, unsigned dropped, bool idle,
                             unsigned finalized)
{
    if (trace_count < TRACE_CAPACITY) {
        trace[trace_count] = (struct reclaim_trace){
            trace_count, ticks, confirmed, dropped, idle, finalized
        };
        ++trace_count;
    }
}

void lifecycle_original_watch(uintptr_t frame, unsigned event)
{
    if (!lifecycle_active || !watched) return;
    size_t i = (frame - page_zone_start(PAGE_ZONE_USER)) / PAGE_SIZE;
    if (i >= sizeof(frame_index) / sizeof(frame_index[0]) || !frame_index[i]) return;
    struct reclaim_frame *f = &original_frames[frame_index[i] - 1];
    if (f->pool_return) return;
    switch (event) {
    case ORIGINAL_BORROW: f->last_borrow = lifecycle_time(); break;
    case ORIGINAL_DROP:
        if (!f->template_drop && !f->pool_return)
            f->template_drop = lifecycle_time();
        break;
    case ORIGINAL_RETURN:
        if (!f->pool_return) f->pool_return = lifecycle_time();
        break;
    }
}

static long watch_frames(struct proc *p, uintptr_t address)
{
    struct reclaim_watch request;
    if (user_copyin(p->pgtbl, &request, address, sizeof(request)) ||
        !request.pages || request.pages > WATCH_CAPACITY ||
        request.begin < USER_HEAP_START || request.begin >= USER_HEAP_END ||
        (request.begin & PAGE_MASK) ||
        request.pages > (USER_HEAP_END - request.begin) / PAGE_SIZE ||
        watched || !lifecycle_active)
        return -22;
    for (unsigned i = 0; i < request.pages; ++i) {
        uintptr_t frame;
        uint64 flags;
        uintptr_t va = request.begin + i * PAGE_SIZE;
        if (!sv39_lookup(p->pgtbl, va, &frame, &flags) || !(flags & PTE_U)) {
            memset(frame_index, 0, sizeof(frame_index));
            return -14;
        }
        frame_index[(frame - page_zone_start(PAGE_ZONE_USER)) / PAGE_SIZE] = i + 1;
        original_frames[i] = (struct reclaim_frame){.index = i, .frame = frame};
    }
    watched = request.pages;
    return 0;
}

static long reclaim_readout(struct proc *p, uintptr_t address, bool batch)
{
    unsigned long start;
    if (user_copyin(p->pgtbl, &start, address, sizeof(start))) return -14;
    if (batch)
        return start < trace_count ? user_copyout(p, address, &trace[start],
                                                  sizeof(trace[start])) : -22;
    return start < watched ? user_copyout(p, address, &original_frames[start],
                                           sizeof(original_frames[start])) : -22;
}

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

static uint64 last_page_event;
static int64_t extra_pages;
static void account_residency(void)
{
    uint64 now = lifecycle_time();
    if (extra_pages > 0)
        template_stats.resident_page_ticks += (now - last_page_event) * (uint64)extra_pages;
    last_page_event = now;
}

void lifecycle_page(bool user, bool allocated)
{
    if (!lifecycle_active)
        return;
    account_residency();
    /* Measured batches release all transient frames before the final snapshot. */
    extra_pages += allocated ? 1 : -1;
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
#ifdef LIFECYCLE_CHECKS
    if (op == PROBE_AUDIT)
        return template_audit(address);
    if (op == PROBE_PROCESS) {
        for (unsigned i = 0; i < NPROC; ++i) {
            const struct proc *slot = proc_slot(i);
            if (slot->state != PROC_UNUSED && (unsigned)slot->pid == address)
                return i + (slot->state == PROC_ZOMBIE ? PROBE_PROCESS_ZOMBIE : 0);
        }
        return -22;
    }
#endif
    if (op == PROBE_WATCH)
        return watch_frames(p, address);
    if (op == PROBE_FRAME_EVENT || op == PROBE_RECLAIM_TRACE)
        return reclaim_readout(p, address, op == PROBE_RECLAIM_TRACE);
    if (op == PROBE_QUIET)
        return template_work_pending();
    if (op == PROBE_RECLAIM)
        return user_copyout(p, address, &reclaim_stats, sizeof(reclaim_stats));
    if (op == PROBE_TEMPLATE)
        return user_copyout(p, address, &template_stats, sizeof(template_stats));
    if (op == PROBE_HEAP_DETAIL) {
        if (lifecycle_active)
            return -22;
        return user_copyout(p, address, &heap_detail, sizeof(heap_detail));
    }
    if (op == PROBE_START) {
        memset(&lifecycle_sample, 0, sizeof(lifecycle_sample));
        memset(&template_stats, 0, sizeof(template_stats));
        memset(&reclaim_stats, 0, sizeof(reclaim_stats));
        if (watched) memset(frame_index, 0, sizeof(frame_index));
        watched = 0;
        trace_count = 0;
        memset(&heap_detail, 0, sizeof(heap_detail));
#ifdef LIFECYCLE_COUNTS
        extra_pages = 0;
        last_page_event = lifecycle_time();
#endif
        lifecycle_active = true;
        return 0;
    }
    if (op != PROBE_STOP && op != PROBE_SNAPSHOT && op != PROBE_BALANCE)
        return -22;
#ifdef LIFECYCLE_COUNTS
    if (lifecycle_active && op != PROBE_BALANCE) account_residency();
#endif
    if (op != PROBE_BALANCE)
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

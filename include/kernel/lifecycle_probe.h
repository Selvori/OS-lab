#ifndef SEELE_LIFECYCLE_PROBE_H
#define SEELE_LIFECYCLE_PROBE_H
#include <kernel/types.h>

#define SYS_LIFECYCLE_PROBE 63
#define PROBE_SNAPSHOT 0
#define PROBE_START 1
#define PROBE_STOP 2
#define PROBE_FAIL_AFTER 3
#define PROBE_MAPPING 4
#define PROBE_HEAP_DETAIL 5
#define PROBE_TEMPLATE 6
#define PROBE_BALANCE 7
#define PROBE_RECLAIM 8
#define PROBE_WATCH 9
#define PROBE_FRAME_EVENT 10
#define PROBE_AUDIT 11
#define PROBE_RECLAIM_TRACE 12
#define PROBE_QUIET 13
#define PROBE_PROCESS 14
#define PROBE_PROCESS_ZOMBIE 256

struct reclaim_stats {
    unsigned long retains, retain_locks, ref_drops, drop_locks;
    unsigned long drops, returned, pending, max_pending;
    unsigned long batches, busy, idle, confirm_max, release_max;
    unsigned long confirm_total, batch_total, batch_max, finalize_ticks, finalize_max, finalized;
    unsigned long slots_matching, slots_other, redirtied, rotation;
    unsigned long peak_backlog, backlog, closed_zombies;
    unsigned long finalize_tables, finalize_kernel, finalize_user;
};

struct reclaim_watch {
    unsigned long begin, pages;
};

struct reclaim_frame {
    unsigned long index, frame, last_borrow, template_drop, pool_return;
};

struct reclaim_trace {
    unsigned long index, ticks, confirmed, released, idle, finalized;
};

#ifdef LIFECYCLE_COUNTS
#define reclaim_count(field, n) do { if (lifecycle_active) reclaim_stats.field += (n); } while (0)
#define reclaim_max(field, n) do { \
    if (lifecycle_active && reclaim_stats.field < (n)) reclaim_stats.field = (n); \
} while (0)
#else
#define reclaim_count(field, n) ((void)(n))
#define reclaim_max(field, n) ((void)(n))
#endif
#ifdef LIFECYCLE_PROBE
extern struct reclaim_stats reclaim_stats;
bool lifecycle_reclaim_recording(void);
void lifecycle_reclaim_trace(uint64 ticks, unsigned confirmed, unsigned dropped, bool idle,
                             unsigned finalized);
void lifecycle_original_watch(uintptr_t frame, unsigned event);
#else
#define lifecycle_original_watch(frame, event) ((void)0)
#endif
#define ORIGINAL_BORROW 0
#define ORIGINAL_DROP 1
#define ORIGINAL_RETURN 2
struct template_stats {
    unsigned long copied_slots, retains, exit_slots, bitmap_words, private_drops;
    unsigned long resident_page_ticks, first_write_ticks, first_writes;
    unsigned long close_scan_branches, close_scan_slots;
    unsigned long released_windows, dropped_refs, release_ticks, max_release_ticks;
};
#ifdef LIFECYCLE_COUNTS
extern struct template_stats template_stats;
#define template_count(field, n) do { if (lifecycle_active) template_stats.field += (n); } while (0)
#else
#define template_count(field, n) ((void)0)
#endif
#define PROBE_HEAP_TABLES 9
#define PROBE_REAP 0
#define PROBE_EXEC 1

struct lifecycle_heap_detail {
    unsigned long checks[2][PROBE_HEAP_TABLES];
};

struct lifecycle_mapping {
    unsigned long address, frame, flags, refs;
};

struct lifecycle_sample {
    unsigned long fork_time, reap_time, copied, exec_bytes;
    unsigned long user_alloc, user_free, user_peak;
    unsigned long kernel_alloc, kernel_free, kernel_peak;
    unsigned long table_alloc, table_free, pte_visits, clone_slots, destroy_slots;
    unsigned long cow_fault, cow_copyout, cow_copy, cow_upgrade;
    unsigned long wait_sleep_time, exec_heap_time, heap_leaves, leaf_checks, leaf_reads;
    unsigned long upper_checks, upper_reads, heap_table_free;
    unsigned long user_left, kernel_left, nodes_left, slots_used, files_used;
};

#ifdef LIFECYCLE_PROBE
#include <kernel/types.h>
struct proc;
extern struct lifecycle_sample lifecycle_sample;
extern struct template_stats template_stats;
extern bool lifecycle_active;

static inline uint64 lifecycle_time(void)
{
    uint64 value;
    __asm__ volatile("rdtime %0" : "=r"(value) :: "memory");
    return value;
}

static inline uint64 lifecycle_begin(void)
{
    return lifecycle_active ? lifecycle_time() : 0;
}

#define lifecycle_end(field, start) do { \
    if (lifecycle_active) lifecycle_sample.field += lifecycle_time() - (start); \
} while (0)

unsigned proc_slots_used(void);
unsigned files_used(void);
long lifecycle_control(struct proc *, unsigned long, uintptr_t);
#else
#define lifecycle_begin() 0
#define lifecycle_end(field, start) ((void)(start))
#endif

#ifdef LIFECYCLE_CHECKS
bool lifecycle_alloc_fails(void);
#endif

#ifdef LIFECYCLE_COUNTS
void lifecycle_page(bool user, bool allocated);
extern bool lifecycle_in_heap;
extern unsigned lifecycle_heap_phase;
void lifecycle_heap_check(uintptr_t va, unsigned level);
#define lifecycle_heap_add(field, delta) do { \
    if (lifecycle_active && lifecycle_in_heap) lifecycle_sample.field += (delta); \
} while (0)
#define lifecycle_heap_scope(enabled) (lifecycle_in_heap = (enabled))
#define lifecycle_heap_phase_set(phase) (lifecycle_heap_phase = (phase))
#define lifecycle_add(field, delta) do { \
    if (lifecycle_active) lifecycle_sample.field += (delta); \
} while (0)
#else
#define lifecycle_page(user, allocated) ((void)0)
#define lifecycle_heap_add(field, delta) ((void)0)
#define lifecycle_heap_scope(enabled) ((void)0)
#define lifecycle_heap_phase_set(phase) ((void)0)
#define lifecycle_heap_check(va, level) ((void)(level))
#define lifecycle_add(field, delta) ((void)0)
#endif

#endif

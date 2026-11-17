#ifndef SEELE_LIFECYCLE_PROBE_H
#define SEELE_LIFECYCLE_PROBE_H

#define SYS_LIFECYCLE_PROBE 63
#define PROBE_SNAPSHOT 0
#define PROBE_START 1
#define PROBE_STOP 2
#define PROBE_FAIL_AFTER 3
#define PROBE_MAPPING 4

struct lifecycle_mapping {
    unsigned long address, frame, flags, refs;
};

struct lifecycle_sample {
    unsigned long fork_time, reap_time, copied, exec_bytes;
    unsigned long user_alloc, user_free, user_peak;
    unsigned long kernel_alloc, kernel_free, kernel_peak;
    unsigned long table_alloc, table_free, pte_visits, clone_slots, destroy_slots;
    unsigned long cow_fault, cow_copyout, cow_copy, cow_upgrade;
    unsigned long user_left, kernel_left, nodes_left, slots_used, files_used;
};

#ifdef LIFECYCLE_PROBE
#include <kernel/types.h>
struct proc;
extern struct lifecycle_sample lifecycle_sample;
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
#define lifecycle_add(field, delta) do { \
    if (lifecycle_active) lifecycle_sample.field += (delta); \
} while (0)
#else
#define lifecycle_page(user, allocated) ((void)0)
#define lifecycle_add(field, delta) ((void)0)
#endif

#endif

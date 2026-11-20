#ifndef BOUNDED_GUEST_H
#define BOUNDED_GUEST_H
#include "lib.h"
#include <kernel/lifecycle_probe.h>

#define PAGE 4096
#define PAGES 2048
#define BASE 0x3000L
#define PROBE(op, arg) call(SYS_LIFECYCLE_PROBE, op, (long)(arg), 0)
#define MAP(va, n) call(SYS_MMAP, (long)(va), n, 0)
#define UNMAP(va, n) call(SYS_MUNMAP, (long)(va), n, 0)
#define CHECK(x) do { if (!(x)) { row("FAIL", (unsigned long[]){__LINE__}, 1); return 90; } } while (0)
#define ROW(label, ...) row(label, (unsigned long[]){__VA_ARGS__}, \
                           sizeof((unsigned long[]){__VA_ARGS__}) / sizeof(unsigned long))

static inline unsigned long now(void)
{
    unsigned long t;
    __asm__ volatile("rdtime %0" : "=r"(t) :: "memory");
    return t;
}

static void row(const char *label, const unsigned long *values, unsigned n)
{
    char buf[280], digits[20];
    unsigned used = 0;
    while (*label) buf[used++] = *label++;
    for (unsigned i = 0; i < n; ++i) {
        unsigned long v = values[i];
        unsigned d = 0;
        do { digits[d++] = '0' + v % 10; v /= 10; } while (v);
        buf[used++] = ' ';
        while (d) buf[used++] = digits[--d];
    }
    buf[used++] = '\n';
    write(1, buf, used);
}

static inline void pause_guest(bool idle)
{
    call(idle ? SYS_SLEEP : SYS_YIELD, idle, 0, 0);
}

static inline bool drain(bool all, bool idle)
{
    unsigned long limit = now() + 15 * 10000000UL;
    while (PROBE(PROBE_QUIET, 0) & (all ? -1 : 1)) {
        if (now() > limit) return false;
        pause_guest(idle);
    }
    return true;
}

static inline bool same(const struct lifecycle_sample *a, const struct lifecycle_sample *b)
{
    return a->user_left == b->user_left && a->kernel_left == b->kernel_left &&
           a->nodes_left == b->nodes_left && a->slots_used == b->slots_used &&
           a->files_used == b->files_used;
}

static inline bool publish(const char *path, int value)
{
    long fd = open(path, OPEN_CREATE | OPEN_WRONLY);
    if (fd < 0) return false;
    long n = write(fd, &value, sizeof(value));
    close(fd);
    return n == sizeof(value);
}

static inline int peek(const char *path)
{
    long fd = open(path, OPEN_RDONLY);
    if (fd < 0) return 0;
    int value;
    long n = read(fd, &value, sizeof(value));
    close(fd);
    return n == sizeof(value) ? value : 0;
}

static inline int await(const char *path, bool idle)
{
    unsigned long limit = now() + 15 * 10000000UL;
    while (now() < limit) {
        int n = peek(path);
        if (n) return n;
        pause_guest(idle);
    }
    return 0;
}

static inline bool wait_for(long pid)
{
    int result;
    return call(SYS_WAIT, (long)&result, 0, 0) == pid && result == 73;
}

static inline void unlink_marker(const char *path)
{
    call(SYS_UNLINK, (long)path, 0, 0);
}

static inline unsigned char original(unsigned i)
{
    return (unsigned char)(i * 17 + 3);
}

static inline struct reclaim_frame frame_event(unsigned i)
{
    struct reclaim_frame f = {.index = i};
    if (PROBE(PROBE_FRAME_EVENT, &f)) f.frame = 0;
    return f;
}

static inline void dump_stats(const struct reclaim_stats *s)
{
    ROW("REF", s->retains, s->retain_locks, s->ref_drops, s->drop_locks,
        s->drops, s->returned, s->pending, s->max_pending, s->peak_backlog, s->backlog);
    ROW("SCH", s->batches, s->busy, s->idle, s->confirm_max, s->release_max,
        s->confirm_total, s->slots_matching, s->slots_other, s->redirtied, s->rotation);
    ROW("FIX", s->batch_total, s->batch_max, s->finalize_ticks, s->finalize_max, s->finalized,
        s->finalize_tables, s->finalize_kernel, s->finalize_user);
}

#endif

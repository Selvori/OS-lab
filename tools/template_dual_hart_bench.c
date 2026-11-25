#include "bounded_guest.h"

static unsigned number(const char *s)
{
    unsigned n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > 90000000U) return 0;
        n = n * 10 + (*s - '0');
    }
    return n;
}

/* Keep a symbol range for the stopped-guest U-mode witness. */
__attribute__((noinline, noclone))
static uint32 calculate(unsigned iterations, uint32 value)
{
    for (unsigned i = 0; i < iterations; ++i)
        value = value * 1664525U + 1013904223U;
    return value;
}

static bool pinned(unsigned i, unsigned parity)
{
    return i == parity || i == 509 + parity || i == 1021 + parity ||
           i == 1533 + parity || i == 2045 + parity;
}

static int run_branch(volatile unsigned char *heap, unsigned iterations, bool pin)
{
    long pid = call(SYS_GETPID, 0, 0, 0);
    CHECK(pid > 0);
    volatile long stack_cookie = pid;
    unsigned char change = pid & 1 ? 0x55 : 0xaa;
    unsigned first = pin ? 1000000 : iterations / 2;
    uint32 value = calculate(first, pid);
    for (unsigned i = 0; i < PAGES; ++i) {
        CHECK(heap[i * PAGE] == original(i));
        if (pin ? !pinned(i, pid & 1) : i == 0 || i == 509)
            heap[i * PAGE] ^= change;
    }
    value = calculate(iterations - first, value);
    for (unsigned i = 0; i < PAGES; ++i) {
        unsigned char want = original(i);
        if (pin ? !pinned(i, pid & 1) : i == 0 || i == 509)
            want ^= change;
        CHECK(heap[i * PAGE] == want);
    }
    /* Exercise both a COW-disposed slot and an original borrowed slot. */
    if (!pin) {
        CHECK(!UNMAP(heap, 1));
        CHECK(!UNMAP(heap + 1021 * PAGE, 1));
    }
    CHECK(stack_cookie == pid);
    return value & 0x7fffffffU;
}

static bool live(long pid)
{
    long slot = PROBE(PROBE_PROCESS, pid);
    return slot >= 0 && slot < PROBE_PROCESS_ZOMBIE;
}

static bool check_batches(const struct reclaim_stats *stats, unsigned policy)
{
    unsigned long released = 0, confirmed = 0, finalized = 0;
    for (unsigned long i = 0; i < stats->batches; ++i) {
        struct reclaim_trace batch = {.index = i};
        if (PROBE(PROBE_RECLAIM_TRACE, &batch) || batch.index != i ||
            batch.confirmed > 32 || batch.released > 8 || batch.finalized > 1)
            return false;
        released += batch.released;
        confirmed += batch.confirmed;
        finalized += batch.finalized;
    }
    if (policy == 9 && (released != PAGES || confirmed != stats->confirm_total ||
                        finalized != 1))
        return false;
    ROW("PAIR BATCH", stats->batches, confirmed, released, finalized,
        stats->confirm_max, stats->release_max);
    return true;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4);
    unsigned policy = number(argv[1]), iterations = number(argv[2]);
    bool pin = argc == 4 && equal(argv[3], "pin");
    bool check = argc == 4 && (pin || equal(argv[3], "check"));
    CHECK((policy == 1 || policy == 9) && iterations);
    CHECK(argc == 3 || (check && (!pin || (policy == 9 && iterations >= 1000000))));

    struct lifecycle_sample before, balance;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before));
    volatile unsigned char *heap = (void *)BASE;
    CHECK(MAP(0, PAGES) == BASE);
    for (unsigned i = 0; i < PAGES; ++i) heap[i * PAGE] = original(i);
    if (check) {
        CHECK(!PROBE(PROBE_START, 0));
        struct reclaim_watch watch = {BASE, PAGES};
        CHECK(!PROBE(PROBE_WATCH, &watch));
    }
    long frozen = call(SYS_FREEZE, policy, 0, 0);
    if (!frozen) return run_branch(heap, iterations, pin);
    CHECK(frozen == 1);
    say("PAIR START\n");
    unsigned long t0 = now();
    long a = call(SYS_SPAWN, 0, 0, 0);
    unsigned long t1 = now();
    long b = call(SYS_SPAWN, 0, 0, 0);
    unsigned long t2 = now();
    CHECK(a > 0 && b == a + 1);
    if (check) CHECK(live(a) && live(b));
    if (pin) CHECK(!UNMAP(heap, PAGES));
    unsigned long tclose = now();
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    unsigned long closed = now();

    if (pin) {
        struct reclaim_stats steady;
        do {
            CHECK(now() - closed < 50000000UL);
            pause_guest(false);
            CHECK(!PROBE(PROBE_RECLAIM, &steady));
        } while (steady.returned < PAGES - 10);
        CHECK(steady.returned == PAGES - 10 && steady.drops == PAGES - 10);
        CHECK(live(a) && live(b));
        ROW("PAIR PIN", a, b, steady.returned, steady.backlog, steady.pending);
    }
    int first, second;
    long w1 = call(SYS_WAIT, (long)&first, 0, 0);
    long w2 = call(SYS_WAIT, (long)&second, 0, 0);
    unsigned long done = now();
    CHECK((w1 == a || w1 == b) && w2 == (w1 == a ? b : a) && first >= 0 && second >= 0);
    unsigned long pending = PROBE(PROBE_QUIET, 0);
    CHECK(drain(true, false));
    unsigned long drained = now();

    if (check) {
        struct reclaim_stats stats;
        struct template_stats template;
        CHECK(!PROBE(PROBE_RECLAIM, &stats) && !PROBE(PROBE_TEMPLATE, &template));
        CHECK(!template.retains && !stats.retains && !stats.pending && !stats.backlog);
        CHECK(stats.finalized == 1 && check_batches(&stats, policy));
        if (pin) {
            for (unsigned i = 0; i < PAGES; ++i) {
                struct reclaim_frame f = frame_event(i);
                CHECK(f.frame && f.template_drop && f.pool_return >= f.template_drop);
            }
        }
        ROW("PAIR OWNERSHIP", template.retains, stats.retains, stats.drops,
            stats.returned, template.private_drops, template.copied_slots);
        CHECK(!PROBE(PROBE_STOP, &balance));
    }
    if (!pin) {
        for (unsigned i = 0; i < PAGES; ++i) CHECK(heap[i * PAGE] == original(i));
        CHECK(!UNMAP(heap, PAGES));
    }
    CHECK(!PROBE(PROBE_SNAPSHOT, &balance) && same(&before, &balance));
    ROW("PAIR RESULT", policy, iterations, a, b, w1, first, w2, second);
    ROW("PAIR TICKS", t1 - t0, t2 - t1, done - t0, closed - tclose,
        done - closed, drained - done, drained - closed, pending);
    say("PAIR PASS\n");
    return 0;
}

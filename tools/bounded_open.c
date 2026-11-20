#include "bounded_guest.h"

static unsigned number(const char *s)
{
    unsigned n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0';
    return n;
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    unsigned policy = number(argv[1]), n = number(argv[2]), stride = number(argv[3]);
    CHECK((policy <= 6 || policy == 8 || policy == 9) && n >= 1 && n <= 64 && stride * 3 < PAGES);
    volatile unsigned char *heap = (void *)BASE;
    struct lifecycle_sample before, after, balance;
    struct template_stats template;
    struct reclaim_stats stats;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before));
    CHECK(MAP(0, PAGES) == BASE);
    for (unsigned i = 0; i < PAGES; ++i) heap[i * PAGE] = original(i);
    CHECK(!PROBE(PROBE_START, 0));
    unsigned long start = now();
    long frozen = call(SYS_FREEZE, policy, 0, 0);
    if (!frozen) {
        for (unsigned i = 0; i < 4; ++i) {
            unsigned k = i * stride;
            CHECK(heap[k * PAGE] == original(k));
            heap[k * PAGE] ^= 0xff;
        }
        return 73;
    }
    CHECK(frozen == 1);
    unsigned long setup = now() - start, spawn = 0, rounds[64];
    for (unsigned i = 0; i < n; ++i) {
        unsigned long t = now();
        long pid = call(SYS_SPAWN, 0, 0, 0);
        spawn += now() - t;
        CHECK(pid > 0 && wait_for(pid));
        rounds[i] = now() - t;
    }
    unsigned long t = now();
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    unsigned long closed = now();
    CHECK(drain(true, false));
    unsigned long end = now();
    CHECK(!PROBE(PROBE_STOP, &after) && !PROBE(PROBE_RECLAIM, &stats));
    CHECK(!PROBE(PROBE_TEMPLATE, &template));
    for (unsigned i = 0; i < PAGES; ++i) CHECK(heap[i * PAGE] == original(i));
    CHECK(!UNMAP(heap, PAGES));
    CHECK(!PROBE(PROBE_SNAPSHOT, &balance) && same(&before, &balance));
    ROW("OPEN", policy, n, stride, setup, spawn, closed - t, end - closed,
        end - start, after.reap_time, template.copied_slots,
        after.user_peak, after.kernel_peak);
    for (unsigned i = 0; i < n; ++i) ROW("ROUND", i, rounds[i]);
    dump_stats(&stats);
    say("OPEN PASS\n");
    return 0;
}

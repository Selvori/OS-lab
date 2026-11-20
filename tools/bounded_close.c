#include "bounded_guest.h"

#define OBSERVATIONS 4096

static int observer(void)
{
    long address = MAP(0, 2 * OBSERVATIONS * sizeof(unsigned long) / PAGE);
    CHECK(address == BASE);
    unsigned long (*data)[2] = (void *)address;
    CHECK(await("/c-arm", false));
    unsigned count = 0;
    volatile unsigned work = 17;
    for (;;) {
        CHECK(count < OBSERVATIONS);
        for (unsigned i = 0; i < 128; ++i) work = work * 33 + i;
        unsigned long t = now();
        data[count][0] = t;
        data[count++][1] = work;
        if (count == 1) CHECK(publish("/c-ready", 1));
        /* On receipt of the end marker, record one last completed unit. */
        if (count > 1 && !(count & 31) && peek("/c-end")) break;
        pause_guest(false);
    }
    CHECK(count < OBSERVATIONS);
    for (unsigned i = 0; i < 128; ++i) work = work * 33 + i;
    data[count][0] = now();
    data[count++][1] = work;
    for (unsigned i = 0; i < count; ++i) ROW("PROGRESS", i, data[i][0], data[i][1]);
    return 73;
}

static int cover(volatile unsigned char *heap, bool idle)
{
    unsigned *stores = (void *)MAP(BASE + PAGES * PAGE, 2);
    CHECK((long)stores == BASE + PAGES * PAGE);
    CHECK(await("/c-go", idle));
    for (unsigned i = 0; i < PAGES; ++i) {
        CHECK(heap[i * PAGE] == original(i));
        unsigned char v = original(i) ^ 255;
        unsigned long t = now();
        heap[i * PAGE] = v;
        stores[i] = now() - t;
    }
    unsigned long written = now();
    CHECK(publish("/c-written", 1));
    CHECK(await("/c-end", true));
    for (unsigned i = 0; i < PAGES; ++i) {
        CHECK(heap[i * PAGE] == (unsigned char)(255U - original(i)));
        ROW("STORE", i, stores[i]);
    }
    ROW("WRITTEN", written);
    return 73;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    unsigned policy = argv[1][0] - '0';
    bool idle = equal(argv[2], "idle");
    struct lifecycle_sample before, starting, steady, after;
    struct reclaim_stats stats;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before));
    long side = idle ? -1 : call(SYS_FORK, 0, 0, 0);
    if (!side) return observer();
    CHECK(idle || side > 0);
    CHECK(MAP(0, PAGES) == BASE);
    volatile unsigned char *heap = (void *)BASE;
    for (unsigned i = 0; i < PAGES; ++i) heap[i * PAGE] = original(i);
    CHECK(!PROBE(PROBE_BALANCE, &starting) && !PROBE(PROBE_START, 0));
    struct reclaim_watch watch = {BASE, PAGES};
    CHECK(!PROBE(PROBE_WATCH, &watch));
    long rc = call(SYS_FREEZE, policy, 0, 0);
    if (!rc) return cover(heap, idle);
    CHECK(rc == 1);
    long child = call(SYS_SPAWN, 0, 0, 0);
    CHECK(child > 0);
    CHECK(idle || (publish("/c-arm", 1) && await("/c-ready", false)));
    unsigned long start = now();
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    unsigned long closed = now();
    CHECK(!UNMAP(heap, PAGES) && publish("/c-go", 1));
    CHECK(await("/c-written", idle) && drain(false, idle));
    unsigned long quiet = now();
    CHECK(!PROBE(PROBE_BALANCE, &steady));
    CHECK(publish("/c-end", 1));
    for (unsigned i = 0; i < (idle ? 1U : 2U); ++i) {
        int status;
        long pid = call(SYS_WAIT, (long)&status, 0, 0);
        CHECK((pid == child || pid == side) && status == 73);
    }
    CHECK(drain(true, idle));
    unsigned long end = now();
    CHECK(!PROBE(PROBE_RECLAIM, &stats));
    CHECK(!PROBE(PROBE_STOP, &after) && same(&before, &after));
    for (unsigned i = 0; i < PAGES; ++i) {
        struct reclaim_frame f = frame_event(i);
        CHECK(f.last_borrow >= closed && f.template_drop >= f.last_borrow &&
              f.pool_return >= f.template_drop && f.pool_return <= quiet);
        ROW("FRAME", i, f.frame, f.last_borrow, f.template_drop, f.pool_return);
    }
    unsigned batches = 0;
    for (;;) {
        struct reclaim_trace r = {.index = batches};
        int result = PROBE(PROBE_RECLAIM_TRACE, &r);
        if (result == -22) break;
        CHECK(!result && r.confirmed <= 32 && r.released <= 8 && r.finalized <= 1);
        ROW("BATCH", r.index, r.ticks, r.confirmed, r.released, r.idle, r.finalized);
        ++batches;
    }
    CHECK(batches < 8192);
    ROW("CLOSE", policy, idle, start, closed, quiet, end, after.user_peak,
        after.kernel_peak, before.user_left - steady.user_left,
        before.user_left - starting.user_left, before.kernel_left - starting.kernel_left);
    dump_stats(&stats);
    unlink_marker("/c-go");
    unlink_marker("/c-written");
    unlink_marker("/c-end");
    if (!idle) {
        unlink_marker("/c-arm");
        unlink_marker("/c-ready");
    }
    say("CLOSE PASS\n");
    return 0;
}

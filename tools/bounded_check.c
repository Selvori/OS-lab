#include "bounded_guest.h"

static bool pinned(unsigned i, bool first, unsigned pages)
{
    if (pages == 1) return false;
    return i == (unsigned)!first || i == 509U + !first || i == 1021U + !first ||
           i == 1533U + !first || i == 2045U + !first;
}

static int branch(volatile unsigned char *heap, unsigned pages)
{
    int pid = await("/b-first", false);
    CHECK(pid > 0);
    bool first = pid == call(SYS_GETPID, 0, 0, 0);
    unsigned char change = first ? 0x55 : 0xaa;
    CHECK(await("/b-go", false));
    for (unsigned i = 0; i < pages; ++i) {
        CHECK(heap[i * PAGE] == original(i));
        if (!pinned(i, first, pages)) heap[i * PAGE] ^= change;
    }
    CHECK(publish(first ? "/b-r1" : "/b-r2", 1));
    CHECK(await("/b-end", true));
    for (unsigned i = 0; i < pages; ++i)
        CHECK(heap[i * PAGE] == (original(i) ^ (pinned(i, first, pages) ? 0 : change)));
    for (unsigned i = 0; i < pages; ++i)
        if (pinned(i, first, pages)) heap[i * PAGE] ^= change;
    return 73;
}

static void observed(unsigned pages, unsigned *drops, unsigned *returns)
{
    *drops = *returns = 0;
    for (unsigned i = 0; i < pages; ++i) {
        struct reclaim_frame f = frame_event(i);
        *drops += f.template_drop != 0;
        *returns += f.pool_return != 0;
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    bool external = equal(argv[1], "ref");
    unsigned policy = external ? 6 : argv[1][0] - '0';
    CHECK(policy <= 9);
    bool per_page = policy == 6 || policy == 8 || policy == 9;
    unsigned pages = external ? 1 : PAGES;
    struct lifecycle_sample before, balance, steady;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before));
    CHECK(MAP(0, pages) == BASE);
    volatile unsigned char *heap = (void *)BASE;
    for (unsigned i = 0; i < pages; ++i) heap[i * PAGE] = original(i);
    CHECK(!PROBE(PROBE_START, 0));
    struct reclaim_watch watch = {BASE, pages};
    CHECK(!PROBE(PROBE_WATCH, &watch));
    long rc = call(SYS_FREEZE, policy, 0, 0);
    if (!rc) return branch(heap, pages);
    CHECK(rc == 1);
    long a = call(SYS_SPAWN, 0, 0, 0), b = call(SYS_SPAWN, 0, 0, 0);
    CHECK(a > 0 && b > 0 && publish("/b-first", a));
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    CHECK(call(SYS_RELEASE, 0, 0, 0) == -22 && call(SYS_SPAWN, 0, 0, 0) == -22);
    if (!external) CHECK(!UNMAP(heap, pages));
    CHECK(publish("/b-go", 1));
    CHECK(await("/b-r1", false) && await("/b-r2", false));
    CHECK(drain(false, false));
    struct reclaim_stats park, after_park;
    CHECK(!PROBE(PROBE_RECLAIM, &park));
    CHECK(!call(SYS_SLEEP, 3, 0, 0) && !PROBE(PROBE_RECLAIM, &after_park));
    CHECK(park.batches == after_park.batches);
    ROW("PARK", park.batches, after_park.batches);
    if (policy == 7) CHECK(PROBE(PROBE_AUDIT, a) == 2038);
    unsigned drops, returns;
    observed(pages, &drops, &returns);
    CHECK(!PROBE(PROBE_BALANCE, &steady));
    ROW("LIVE", policy, pages, drops, returns, before.user_left - steady.user_left);
    if (external) {
        struct lifecycle_mapping m = {.address = BASE};
        struct reclaim_frame f = frame_event(0);
        CHECK(drops == 1 && returns == 0 && *heap == original(0));
        CHECK(!PROBE(PROBE_MAPPING, &m) && m.refs == 1 && m.frame == f.frame);
        CHECK(!UNMAP(heap, pages));
        struct reclaim_frame released = frame_event(0);
        CHECK(f.frame == released.frame && released.pool_return > f.template_drop);
        ROW("IDENTITY", released.frame, released.template_drop, released.pool_return);
    } else {
        CHECK(drops == (per_page ? 2038U : 0) && returns == drops);
        for (unsigned i = 0; i < pages; ++i) {
            bool pin = pinned(i, true, pages) || pinned(i, false, pages);
            bool expected = per_page && !pin;
            struct reclaim_frame f = frame_event(i);
            CHECK(f.frame && !!f.pool_return == expected && !!f.template_drop == expected);
            if (pin) ROW("PIN", i, f.frame, f.template_drop, f.pool_return);
        }
    }
    CHECK(publish("/b-end", 1));
    int status;
    long first = call(SYS_WAIT, (long)&status, 0, 0);
    CHECK((first == a || first == b) && status == 73);
    long second = call(SYS_WAIT, (long)&status, 0, 0);
    CHECK(second == (first == a ? b : a) && status == 73);
    ROW("WAIT", first, second);
    CHECK(drain(true, false));
    observed(pages, &drops, &returns);
    CHECK(returns == pages);
    struct reclaim_stats stats;
    CHECK(!PROBE(PROBE_RECLAIM, &stats));
    CHECK(!stats.pending && !stats.backlog && stats.confirm_max <= 32 && stats.release_max <= 8);
    if (per_page && !external) CHECK(stats.redirtied);
    if (policy == 8 || policy == 9) {
        unsigned long dropped = 0, confirmed = 0, finalized = 0;
        for (unsigned long i = 0; i < stats.batches; ++i) {
            struct reclaim_trace batch = {.index = i};
            CHECK(!PROBE(PROBE_RECLAIM_TRACE, &batch) && batch.index == i);
            CHECK(batch.confirmed <= 32 && batch.released <= 8 && batch.finalized <= 1);
            dropped += batch.released;
            confirmed += batch.confirmed;
            finalized += batch.finalized;
        }
        CHECK(dropped == PAGES && confirmed == stats.confirm_total && finalized == 1);
        ROW("TRACE", stats.batches, confirmed, dropped, finalized);
    }
    CHECK(!PROBE(PROBE_STOP, &balance) && same(&before, &balance));
    ROW("FINAL", drops, returns, balance.user_left, balance.kernel_left,
        balance.nodes_left, balance.slots_used, balance.files_used);
    dump_stats(&stats);
    unlink_marker("/b-go");
    unlink_marker("/b-first");
    unlink_marker("/b-r1");
    unlink_marker("/b-r2");
    unlink_marker("/b-end");
    say("CHECK PASS\n");
    return 0;
}

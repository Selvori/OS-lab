#include "bounded_guest.h"

static int check_terminal(unsigned pages, unsigned returned)
{
    struct reclaim_stats stats;
    CHECK(!PROBE(PROBE_RECLAIM, &stats));
    CHECK(!stats.pending && !stats.backlog && stats.drops == pages);
    CHECK(stats.returned == returned && stats.finalized == 1);
    CHECK(!stats.confirm_total && !stats.confirm_max && stats.release_max == 8);
    unsigned long drops = 0, finalizations = 0;
    for (unsigned long i = 0; i < stats.batches; ++i) {
        struct reclaim_trace batch = {.index = i};
        CHECK(!PROBE(PROBE_RECLAIM_TRACE, &batch) && batch.index == i);
        CHECK(!batch.confirmed && batch.released <= 8 && batch.finalized <= 1);
        drops += batch.released;
        finalizations += batch.finalized;
    }
    CHECK(drops == pages && finalizations == 1);
    ROW("TRACE", stats.batches, stats.confirm_total, drops, finalizations);
    dump_stats(&stats);
    return 0;
}

static int check_owner(unsigned policy)
{
    struct lifecycle_sample before, balance;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before));
    CHECK(MAP(0, PAGES) == BASE);
    CHECK(!PROBE(PROBE_START, 0));
    struct reclaim_watch watch = {BASE, PAGES};
    CHECK(!PROBE(PROBE_WATCH, &watch));
    CHECK(call(SYS_FREEZE, policy, 0, 0) == 1);
    for (unsigned i = 0; i < PAGES; ++i) {
        struct lifecycle_mapping m = {.address = BASE + i * PAGE};
        CHECK(!PROBE(PROBE_MAPPING, &m) && m.refs == 2);
        CHECK(m.frame == frame_event(i).frame);
    }
    CHECK(!call(SYS_RELEASE, 0, 0, 0) && drain(true, false));
    if (policy == 9) CHECK(!check_terminal(PAGES, 0));
    for (unsigned i = 0; i < PAGES; ++i) {
        struct lifecycle_mapping m = {.address = BASE + i * PAGE};
        struct reclaim_frame f = frame_event(i);
        CHECK(!PROBE(PROBE_MAPPING, &m) && m.refs == 1);
        CHECK(m.frame == f.frame && !f.pool_return);
        if (policy != 1) CHECK(f.template_drop);
    }
    CHECK(!UNMAP(BASE, PAGES));
    for (unsigned i = 0; i < PAGES; ++i) {
        struct reclaim_frame f = frame_event(i);
        CHECK(f.frame && f.pool_return > f.template_drop);
    }
    CHECK(!PROBE(PROBE_STOP, &balance) && same(&before, &balance));
    ROW("OWNER", policy, PAGES, PAGES, 0, PAGES);
    say("OWNER PASS\n");
    return 0;
}

static bool hole(unsigned i)
{
    /* Leading and trailing holes, a 64-slot boundary and two 2 MiB boundaries. */
    return i == 0 || (i >= 59 && i < 68) || (i >= 506 && i < 516) ||
           (i >= 1019 && i < 1024) || i == PAGES - 1;
}

static int check_sparse(void)
{
    struct lifecycle_sample before, balance;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before));
    CHECK(MAP(0, PAGES) == BASE);
    CHECK(!PROBE(PROBE_START, 0));
    struct reclaim_watch watch = {BASE, PAGES};
    CHECK(!PROBE(PROBE_WATCH, &watch));

    /* Watch the original identities before punching holes so the event log
     * distinguishes pages freed before freeze from those dropped at close. */
    for (unsigned i = 0; i < PAGES;) {
        unsigned start = i++;
        while (i < PAGES && hole(i) == hole(start)) ++i;
        if (hole(start)) CHECK(!UNMAP(BASE + start * PAGE, i - start));
    }
    CHECK(call(SYS_FREEZE, 9, 0, 0) == 1);
    unsigned held = 0;
    for (unsigned i = 0; i < PAGES;) {
        unsigned start = i++;
        while (i < PAGES && hole(i) == hole(start)) ++i;
        if (hole(start)) continue;
        CHECK(!UNMAP(BASE + start * PAGE, i - start));
        held += i - start;
    }
    for (unsigned i = 0; i < PAGES; ++i) {
        struct reclaim_frame f = frame_event(i);
        CHECK(f.frame && !f.template_drop && !!f.pool_return == hole(i));
    }
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    CHECK(call(SYS_SPAWN, 0, 0, 0) == -22 && drain(true, false));
    CHECK(held == 2022 && !check_terminal(held, held));
    for (unsigned i = 0; i < PAGES; ++i) {
        struct reclaim_frame f = frame_event(i);
        CHECK(f.frame && f.pool_return > f.template_drop);
        CHECK(!!f.template_drop == !hole(i));
    }
    CHECK(!PROBE(PROBE_STOP, &balance) && same(&before, &balance));
    ROW("SPARSE", PAGES, PAGES - held, held, held);
    say("SPARSE PASS\n");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (equal(argv[1], "ref1")) return check_owner(1);
    if (equal(argv[1], "ref6")) return check_owner(6);
    if (equal(argv[1], "ref8")) return check_owner(8);
    if (equal(argv[1], "ref9")) return check_owner(9);
    if (equal(argv[1], "sparse9")) return check_sparse();
    CHECK(equal(argv[1], "6") || equal(argv[1], "8") || equal(argv[1], "9"));
    unsigned policy = argv[1][0] - '0';
    struct lifecycle_sample before, balance;
    struct reclaim_stats competed, before_sleep, ordinary, finished;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before));

    long child = call(SYS_FORK, 0, 0, 0);
    CHECK(child >= 0);
    if (!child) {
        CHECK(publish("/b-yield-ran", 1));
        CHECK(await("/b-yield-exit", false));
        return 73;
    }
    /* Synchronize after the child's initial filesystem write. It remains
     * runnable, polling and yielding, until the parent releases it. */
    CHECK(await("/b-yield-ran", false));

    CHECK(MAP(0, PAGES) == BASE);
    CHECK(!PROBE(PROBE_START, 0));
    CHECK(call(SYS_FREEZE, policy, 0, 0) == 1);
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    unsigned long t = now();
    CHECK(!call(SYS_YIELD, 0, 0, 0));
    unsigned long contention = now() - t;
    CHECK(!PROBE(PROBE_RECLAIM, &competed));
    CHECK(competed.busy >= 2 && !competed.idle && competed.drops < PAGES);
    CHECK(publish("/b-yield-exit", 1) && wait_for(child));

    CHECK(!PROBE(PROBE_RECLAIM, &before_sleep));
    t = now();
    CHECK(!call(SYS_SLEEP, 0, 0, 0));
    unsigned long sleep_zero = now() - t;
    CHECK(!PROBE(PROBE_RECLAIM, &ordinary));
    CHECK(ordinary.busy > before_sleep.busy && ordinary.drops < PAGES);
    CHECK(ordinary.idle == before_sleep.idle);

    unsigned long limit = now() + 10000000UL, max_yield = 0, yields = 0;
    while (PROBE(PROBE_QUIET, 0)) {
        CHECK(now() < limit);
        t = now();
        CHECK(!call(SYS_YIELD, 0, 0, 0));
        unsigned long elapsed = now() - t;
        if (elapsed > max_yield) max_yield = elapsed;
        ++yields;
    }
    CHECK(!PROBE(PROBE_RECLAIM, &finished));
    CHECK(finished.batches == 256 && finished.drops == PAGES && !finished.returned);
    CHECK(finished.confirm_max <= 32 && finished.release_max == 8 && finished.finalized == 1);
    CHECK(yields && (policy == 6 ? finished.idle == ordinary.idle :
                     finished.idle > ordinary.idle && yields < 256));

    unsigned long normal_max = 0;
    for (unsigned i = 0; i < 32; ++i) {
        t = now();
        CHECK(!call(SYS_YIELD, 0, 0, 0));
        unsigned long elapsed = now() - t;
        if (elapsed > normal_max) normal_max = elapsed;
    }
    CHECK(!UNMAP(BASE, PAGES));
    CHECK(!PROBE(PROBE_STOP, &balance) && same(&before, &balance));
    ROW("YIELD", policy, contention, sleep_zero, max_yield, normal_max, yields,
        competed.busy, competed.idle, ordinary.idle);
    dump_stats(&finished);
    unlink_marker("/b-yield-ran");
    unlink_marker("/b-yield-exit");
    say("YIELD PASS\n");
    return 0;
}

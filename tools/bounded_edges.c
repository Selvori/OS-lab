#include "bounded_guest.h"

#define EDGE 0x3ff000L
#define GAP (EDGE - PAGE)

static int private_or_shared(volatile unsigned char *p)
{
    bool first = await("/e-first", false) == call(SYS_GETPID, 0, 0, 0);
    CHECK(p[0] == 19 && p[PAGE] == 29);
    if (first) {
        p[0] = 37;
        CHECK(MAP(GAP, 1) == GAP && !*(volatile unsigned char *)GAP);
        CHECK(!UNMAP(GAP, 1));
    }
    CHECK(publish(first ? "/e-r1" : "/e-r2", 1));
    if (first) {
        CHECK(await("/e-death", true) && p[0] == 37 && p[PAGE] == 29);
        return 73;
    }
    CHECK(await("/e-drop", true));
    CHECK(!UNMAP(p, 1) && MAP(p, 1) == EDGE && !p[0]);
    p[0] = 43;
    CHECK(publish("/e-private", 1));
    CHECK(await("/e-last", true) && p[PAGE] == 29);
    p[PAGE] = 47;
    CHECK(publish("/e-both", 1));
    CHECK(await("/e-exec", true) && p[0] == 43 && p[PAGE] == 47);
    const char *argv[] = {"/bin/lifejob", "work", 0};
    return call(SYS_EXEC, (long)argv[0], (long)argv, 0);
}

static int audit(void)
{
    volatile unsigned char *p = (void *)EDGE;
    struct lifecycle_sample before, after;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before) && MAP(EDGE, 2) == EDGE);
    p[0] = 19;
    p[PAGE] = 29;
    CHECK(!PROBE(PROBE_START, 0));
    long frozen = call(SYS_FREEZE, 7, 0, 0);
    if (!frozen) return private_or_shared(p);
    CHECK(frozen == 1);
    long a = call(SYS_SPAWN, 0, 0, 0), b = call(SYS_SPAWN, 0, 0, 0);
    CHECK(a > 0 && b > 0 && publish("/e-first", a));
    CHECK(await("/e-r1", false) && await("/e-r2", false));
    int slot = PROBE(PROBE_PROCESS, a);
    CHECK(slot >= 0 && slot < PROBE_PROCESS_ZOMBIE);
    CHECK(!call(SYS_RELEASE, 0, 0, 0) && !UNMAP(p, 2) && drain(false, false));
    CHECK(PROBE(PROBE_AUDIT, b) == 0);
    CHECK(publish("/e-drop", 1) && await("/e-private", false) && drain(false, false));
    CHECK(PROBE(PROBE_AUDIT, b) == 1);
    CHECK(publish("/e-death", 1));
    unsigned long limit = now() + 10000000;
    while (PROBE(PROBE_PROCESS, a) == slot && now() < limit) pause_guest(false);
    CHECK(PROBE(PROBE_PROCESS, a) == slot + PROBE_PROCESS_ZOMBIE);
    CHECK(publish("/e-last", 1) && await("/e-both", false) && drain(false, false));
    CHECK(PROBE(PROBE_AUDIT, b) == 1);
    CHECK(wait_for(a));
    long replacement = call(SYS_FORK, 0, 0, 0);
    if (!replacement) return 73;
    CHECK(replacement > 0 && PROBE(PROBE_PROCESS, replacement) == slot);
    CHECK(drain(false, false) && PROBE(PROBE_AUDIT, b) == 2);
    ROW("AUDIT", slot, replacement, 0, 1, 1, 2);
    CHECK(wait_for(replacement) && publish("/e-exec", 1) && wait_for(b) && drain(true, false));
    struct reclaim_stats stats;
    CHECK(!PROBE(PROBE_RECLAIM, &stats) && stats.drops == 2 && stats.returned == 2);
    CHECK(!PROBE(PROBE_STOP, &after) && same(&before, &after));
    const char *markers[] = {"/e-first", "/e-r1", "/e-r2", "/e-drop", "/e-private",
                             "/e-death", "/e-last", "/e-both", "/e-exec"};
    for (unsigned i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i)
        unlink_marker(markers[i]);
    return 0;
}

static int lifetime(void)
{
    struct lifecycle_sample before, after;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before));
    CHECK(!PROBE(PROBE_START, 0));
    long creator = call(SYS_FORK, 0, 0, 0);
    if (!creator) {
        CHECK(MAP(0, 100) == BASE);
        volatile unsigned char *p = (void *)BASE;
        *p = 81;
        struct reclaim_watch watch = {BASE, 100};
        CHECK(!PROBE(PROBE_WATCH, &watch));
        long frozen = call(SYS_FREEZE, 6, 0, 0);
        if (!frozen) {
            CHECK(*p == 81);
            return 73;
        }
        CHECK(frozen == 1);
        long branch = call(SYS_SPAWN, 0, 0, 0);
        CHECK(branch > 0 && publish("/e-zpid", branch));
        unsigned long limit = now() + 10000000;
        while (PROBE(PROBE_PROCESS, branch) < PROBE_PROCESS_ZOMBIE && now() < limit)
            pause_guest(false);
        CHECK(PROBE(PROBE_PROCESS, branch) >= PROBE_PROCESS_ZOMBIE);
        /* proc_finish implicitly closes the creator's handle and orphans the zombie. */
        return 73;
    }
    CHECK(creator > 0 && wait_for(creator));
    /* The live shell is blocked in wait for us. Reparenting its orphan
     * wakes it so it can reap the zombie while this process stays alive. */
    int branch = peek("/e-zpid");
    CHECK(branch > 0);
    unsigned long limit = now() + 10000000;
    while (PROBE(PROBE_PROCESS, branch) >= 0 && now() < limit) pause_guest(false);
    CHECK(PROBE(PROBE_PROCESS, branch) == -22 && drain(true, false));
    struct reclaim_stats stats;
    CHECK(!PROBE(PROBE_RECLAIM, &stats) && stats.closed_zombies == 1 &&
          !stats.pending && stats.max_pending == 1 && stats.finalized == 1 &&
          stats.drops == 100 && stats.returned <= 100);
    for (unsigned i = 0; i < 100; ++i) {
        struct reclaim_frame f = frame_event(i);
        CHECK(f.template_drop && f.pool_return >= f.template_drop);
    }
    CHECK(!PROBE(PROBE_STOP, &after) && same(&before, &after));
    ROW("ORPHAN", branch, stats.closed_zombies, stats.drops, stats.returned);
    unlink_marker("/e-zpid");
    return 0;
}

static int rotation(void)
{
    struct lifecycle_sample before, after;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before) && MAP(0, PAGES) == BASE);
    volatile unsigned char *p = (void *)BASE;
    p[0] = 91;
    CHECK(!PROBE(PROBE_START, 0));
    CHECK(call(SYS_FREEZE, 6, 0, 0) == 1);
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    CHECK(call(SYS_FREEZE, 6, 0, 0) == 1);
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    CHECK(drain(true, true) && p[0] == 91);
    struct reclaim_stats stats;
    CHECK(!PROBE(PROBE_RECLAIM, &stats));
    CHECK(stats.max_pending == 2 && stats.rotation && stats.idle && stats.finalized == 2 &&
          stats.drops == 2 * PAGES && !stats.returned &&
          stats.confirm_max == 32 && stats.release_max == 8 && !stats.backlog);
    CHECK(!UNMAP(p, PAGES) && !PROBE(PROBE_STOP, &after) && same(&before, &after));
    dump_stats(&stats);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    int result;
    if (equal(argv[1], "audit")) result = audit();
    else if (equal(argv[1], "rotation")) result = rotation();
    else if (equal(argv[1], "lifetime")) result = lifetime();
    else return 2;
    if (result) return result;
    say("EDGES PASS\n");
    return 0;
}

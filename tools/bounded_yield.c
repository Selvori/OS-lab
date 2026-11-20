#include "bounded_guest.h"

int main(int argc, char **argv)
{
    CHECK(argc == 2 && (equal(argv[1], "6") || equal(argv[1], "8")));
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

    CHECK(MAP(0, PAGES) == BASE);
    CHECK(!PROBE(PROBE_START, 0));
    CHECK(call(SYS_FREEZE, policy, 0, 0) == 1);
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    unsigned long t = now();
    CHECK(!call(SYS_YIELD, 0, 0, 0));
    unsigned long contention = now() - t;
    CHECK(peek("/b-yield-ran"));
    CHECK(!PROBE(PROBE_RECLAIM, &competed));
    CHECK(competed.busy && !competed.idle && competed.drops < PAGES);
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

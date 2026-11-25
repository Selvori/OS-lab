#include "bounded_guest.h"

static bool zombie(long pid)
{
    unsigned long deadline = now() + 10000000UL;
    do {
        if (PROBE(PROBE_PROCESS, pid) >= PROBE_PROCESS_ZOMBIE) return true;
        pause_guest(false);
    } while (now() < deadline);
    return false;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (equal(argv[1], "replaced")) {
        CHECK(publish("/ld-execed", 1) && await("/ld-resume", false));
        return 73;
    }
    bool exec = equal(argv[1], "exec");
    bool overlap = equal(argv[1], "overlap");
    CHECK(exec || overlap || equal(argv[1], "wait"));

    volatile unsigned char *heap = (void *)BASE;
    struct lifecycle_sample before, after;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before));
    CHECK(MAP(0, PAGES) == BASE);
    heap[0] = 0x37;
    heap[(PAGES - 1) * PAGE] = 0x96;
    if (overlap) CHECK(!PROBE(PROBE_START, 0));

    long frozen = call(SYS_FREEZE, 9, 0, 0);
    if (!frozen) {
        CHECK(heap[0] == 0x37 && heap[(PAGES - 1) * PAGE] == 0x96);
        CHECK(await("/ld-go", false));
        if (exec) {
            const char *replacement[] = {"latewait", "replaced", NULL};
            call(SYS_EXEC, (long)"/bin/latewait", (long)replacement, 0);
            return 91;
        }
        return 73;
    }
    CHECK(frozen == 1);
    long a = call(SYS_SPAWN, 0, 0, 0);
    long b = exec || overlap ? 0 : call(SYS_SPAWN, 0, 0, 0);
    CHECK(a > 0 && (exec || overlap || b > 0));
    CHECK(!call(SYS_RELEASE, 0, 0, 0));
    if (overlap) {
        CHECK(call(SYS_FREEZE, 9, 0, 0) == 1);
        CHECK(!call(SYS_RELEASE, 0, 0, 0));
        CHECK((PROBE(PROBE_QUIET, 0) >> 1) == 2);
    }
    CHECK(publish("/ld-go", 1));

    if (exec) {
        CHECK(await("/ld-execed", false) && drain(true, false));
        long slot = PROBE(PROBE_PROCESS, a);
        CHECK(slot >= 0 && slot < PROBE_PROCESS_ZOMBIE);
        CHECK(publish("/ld-resume", 1) && wait_for(a));
        ROW("LIVE EXEC", a, slot);
    } else if (overlap) {
        CHECK(wait_for(a) && drain(true, false));
        struct reclaim_stats stats;
        CHECK(!PROBE(PROBE_RECLAIM, &stats) && stats.finalized == 2 && stats.max_pending == 2);
        CHECK(stats.drops == 2 * PAGES && stats.returned == 0 && stats.rotation > 0);
        CHECK(stats.confirm_max <= 32 && stats.release_max <= 8);
        CHECK(!PROBE(PROBE_STOP, &after));
        ROW("OVERLAP", stats.drops, stats.returned, stats.finalized, stats.rotation,
            stats.confirm_max, stats.release_max);
    } else {
        CHECK(zombie(a) && zombie(b));
        int sa, sb;
        unsigned long start = now();
        long one = call(SYS_WAIT, (long)&sa, 0, 0);
        unsigned long middle = now();
        long two = call(SYS_WAIT, (long)&sb, 0, 0);
        unsigned long stop = now();
        long pending = PROBE(PROBE_QUIET, 0);
        CHECK(drain(true, false));
        unsigned long drained = now();
        CHECK(((one == a && two == b) || (one == b && two == a)) && sa == 73 && sb == 73);
        ROW("TIMED WAIT", middle - start, stop - middle, drained - stop, pending);
    }

    CHECK(heap[0] == 0x37 && heap[(PAGES - 1) * PAGE] == 0x96);
    CHECK(!UNMAP(heap, PAGES) && !PROBE(PROBE_SNAPSHOT, &after) && same(&before, &after));
    unlink_marker("/ld-go");
    if (exec) {
        unlink_marker("/ld-execed");
        unlink_marker("/ld-resume");
    }
    say("DETACH PASS\n");
    return 0;
}

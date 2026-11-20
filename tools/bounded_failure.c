#include "bounded_guest.h"

#define EDGE 0x3ff000L
#define GAP (EDGE - 2 * PAGE)

static int fail_then_commit(volatile unsigned char *p, unsigned policy)
{
    CHECK(await("/f-go", true));
    struct lifecycle_sample before, after;
    CHECK(!PROBE(PROBE_BALANCE, &before));
    CHECK(!PROBE(PROBE_BALANCE, &before));
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(!PROBE(PROBE_FAIL_AFTER, i) && UNMAP(p, 2) == -12);
        CHECK(!PROBE(PROBE_BALANCE, &after) && same(&before, &after));
    }
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(!PROBE(PROBE_FAIL_AFTER, i) && MAP(GAP, 2) == -12);
        CHECK(!PROBE(PROBE_BALANCE, &after) && same(&before, &after));
    }
    const char *argv[] = {"/bin/lifejob", "work", 0};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(!PROBE(PROBE_FAIL_AFTER, i));
        CHECK(call(SYS_EXEC, (long)argv[0], (long)argv, 0) == -12);
        CHECK(!PROBE(PROBE_BALANCE, &after) && same(&before, &after));
    }
    long fd = open("/etc/motd", OPEN_RDONLY);
    CHECK(fd >= 0);
    CHECK(!PROBE(PROBE_BALANCE, &before));
    void *cross = (void *)(EDGE + PAGE - 2);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(!PROBE(PROBE_FAIL_AFTER, i) && read(fd, cross, 4) == -12);
        CHECK(!p[PAGE - 2] && p[PAGE] == 29);
        CHECK(!PROBE(PROBE_BALANCE, &after) && same(&before, &after));
    }
    CHECK(*p == 19 && p[PAGE] == 29 && publish("/f-rolled", 1));
    CHECK(await("/f-commit", true));
    CHECK(read(fd, cross, 4) == 4 && p[PAGE - 2] == 'W' && p[PAGE] == 'l');
    close(fd);
    CHECK(publish("/f-set", 1) && await("/f-exec", true));
    CHECK(p[PAGE - 2] == 'W' && p[PAGE] == 'l');
    if (policy != 3) return call(SYS_EXEC, (long)argv[0], (long)argv, 0);
    return 73;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    unsigned policy = argv[1][0] - '0';
    CHECK(policy == 3 || policy == 6 || policy == 7);
    struct lifecycle_sample before, stable, after;
    CHECK(!PROBE(PROBE_SNAPSHOT, &before) && MAP(EDGE, 2) == EDGE);
    volatile unsigned char *p = (void *)EDGE;
    *p = 19;
    p[PAGE] = 29;
    CHECK(!PROBE(PROBE_BALANCE, &stable));
    struct lifecycle_mapping mapping = {.address = EDGE};
    CHECK(!PROBE(PROBE_MAPPING, &mapping));
    for (unsigned i = 0; i < (policy == 3 ? 8U : 9U); ++i) {
        CHECK(!PROBE(PROBE_FAIL_AFTER, i) && call(SYS_FREEZE, policy, 0, 0) == -12);
        CHECK(!PROBE(PROBE_BALANCE, &after) && same(&stable, &after));
        struct lifecycle_mapping current = {.address = EDGE};
        CHECK(!PROBE(PROBE_MAPPING, &current) &&
              current.frame == mapping.frame && current.flags == mapping.flags &&
              current.refs == mapping.refs);
    }
    CHECK(!PROBE(PROBE_START, 0));
    struct reclaim_watch watch = {EDGE, 2};
    CHECK(!PROBE(PROBE_WATCH, &watch));
    long frozen = call(SYS_FREEZE, policy, 0, 0);
    if (!frozen) return fail_then_commit(p, policy);
    CHECK(frozen == 1);
    CHECK(!PROBE(PROBE_BALANCE, &stable) && !PROBE(PROBE_BALANCE, &stable));
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(!PROBE(PROBE_FAIL_AFTER, i) && call(SYS_SPAWN, 0, 0, 0) == -12);
        CHECK(!PROBE(PROBE_BALANCE, &after) && same(&stable, &after));
    }
    long pid = call(SYS_SPAWN, 0, 0, 0);
    CHECK(pid > 0 && !call(SYS_RELEASE, 0, 0, 0) && !UNMAP(p, 2));
    CHECK(publish("/f-go", 1) && await("/f-rolled", false) && drain(false, false));
    if (policy == 7) CHECK(PROBE(PROBE_AUDIT, pid) == 0);
    CHECK(!frame_event(0).pool_return && !frame_event(1).pool_return);
    CHECK(publish("/f-commit", 1) && await("/f-set", false) && drain(false, false));
    if (policy == 7) CHECK(PROBE(PROBE_AUDIT, pid) == 2);
    if (policy == 6) CHECK(frame_event(0).pool_return && frame_event(1).pool_return);
    CHECK(publish("/f-exec", 1) && wait_for(pid) && drain(true, false));
    CHECK(frame_event(0).pool_return && frame_event(1).pool_return);
    CHECK(!PROBE(PROBE_STOP, &after) && same(&before, &after));
    unlink_marker("/f-go");
    unlink_marker("/f-rolled");
    unlink_marker("/f-commit");
    unlink_marker("/f-set");
    unlink_marker("/f-exec");
    ROW("ROLLBACK", policy, policy == 3 ? 8 : 9, 8, 2, 3, 3, 4);
    say("FAILURE PASS\n");
    return 0;
}

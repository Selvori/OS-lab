#include "lib.h"
#include <kernel/lifecycle_probe.h>

#define PAGE 4096UL
#define PROBE(op, arg) call(SYS_LIFECYCLE_PROBE, (op), (long)(arg), 0)
#define COW (1UL << 8)
#define WRITE (1UL << 2)

static int mapping(volatile unsigned char *page, bool shared)
{
    struct lifecycle_mapping m = { .address = (unsigned long)page };
    unsigned long expected = shared ? COW : WRITE;
    return PROBE(PROBE_MAPPING, &m) == 0 &&
           (m.flags & (COW | WRITE)) == expected && (shared ? m.refs >= 2 : m.refs == 1);
}

static bool balances(struct lifecycle_sample *before, struct lifecycle_sample *after)
{
    return before->user_left == after->user_left &&
           before->kernel_left == after->kernel_left &&
           before->nodes_left == after->nodes_left &&
           before->slots_used == after->slots_used && before->files_used == after->files_used;
}

enum { EARLY, HOLE, WINDOW, OOM, WRITE_BOUNDARY };

static int child(volatile unsigned char *base, unsigned scenario)
{
    volatile unsigned char *a = base, *b = base + PAGE, *c = base + 2 * PAGE;
    volatile unsigned char *d = base + 3 * PAGE, *e = base + 4 * PAGE;
    volatile unsigned char *f = base + 5 * PAGE, *g = base + 6 * PAGE;
    if (!mapping(a, true))
        return 11; /* Also make the child stack private before starting the counters. */
    if (scenario == WRITE_BOUNDARY)
        *c = 7;
    if (PROBE(PROBE_START, 0))
        return 12;
    *a = 11;
    if (scenario == OOM && PROBE(PROBE_FAIL_AFTER, 2))
        return 13; /* Demand page and first extra succeed; the second extra fails. */
    *b = 12;
    struct lifecycle_sample s;
    if (PROBE(PROBE_STOP, &s) || s.cow_fault != 2 || s.cow_copyout != 0 ||
        s.cow_upgrade != 0 || *a != 11 || *b != 12 ||
        !mapping(a, false) || !mapping(b, false))
        return 14;
    unsigned expected = scenario == EARLY ? 4 : (scenario == HOLE || scenario == OOM);
    if (s.cow_copy != 2 + expected || s.copied != (2 + expected) * PAGE)
        return 15;
    switch (scenario) {
    case EARLY:
        if (!mapping(c, false) || !mapping(d, false) || !mapping(e, false) ||
            !mapping(f, false) || !mapping(g, true) ||
            *c != 3 || *d != 4 || *e != 5 || *f != 6 || c[PAGE - 1] != 93)
            return 16;
        /* The parent of this grandchild just predicted g. The new child must
           build its own history even when it starts at exactly that address. */
        {
            long pid = call(SYS_FORK, 0, 0, 0);
            if (pid == 0) {
                *g = 17;
                call(SYS_EXIT, mapping(g + PAGE, true) ? 0 : 24, 0, 0);
                return 24;
            }
            int status = -1;
            if (pid < 0 || call(SYS_WAIT, (long)&status, 0, 0) != pid || status || *g != 7)
                return 24;
        }
        break;
    case HOLE:
        if (!mapping(c, false) || !mapping(e, true) || !mapping(f, true))
            return 17;
        *e = 15;
        if (*e != 15 || !mapping(f, true))
            return 18;
        break;
    case WINDOW:
        if (!mapping(c, true))
            return 19;
        *c = 13;
        if (!mapping(d, true))
            return 20;
        *d = 14;
        if (!mapping(e, false))
            return 21;
        break;
    case OOM:
        if (!mapping(c, false) || !mapping(d, true) || !mapping(e, true))
            return 22;
        *d = 14; /* Failed speculative allocation was one-shot. */
        if (*d != 14 || !mapping(d, false))
            return 23;
        break;
    case WRITE_BOUNDARY:
        if (!mapping(c, false) || *c != 7 || !mapping(d, true))
            return 25;
        break;
    }
    return 73;
}

int main(void)
{
    for (unsigned scenario = EARLY; scenario <= WRITE_BOUNDARY; ++scenario) {
        struct lifecycle_sample before, after;
        if (PROBE(PROBE_SNAPSHOT, &before))
            return 1;
        long hint = scenario == WINDOW ? 0x1fe000 : 0;
        long address = call(SYS_MMAP, hint, 8, 0);
        if (address <= 0 || (hint && address != hint))
            return 2;
        volatile unsigned char *base = (void *)address;
        for (unsigned i = 0; i < 8; ++i) {
            base[i * PAGE] = (unsigned char)(i + 1);
            base[i * PAGE + PAGE - 1] = (unsigned char)(i + 91);
        }
        if (scenario == HOLE && call(SYS_MUNMAP, address + 3 * PAGE, 1, 0))
            return 3;
        long pid = call(SYS_FORK, 0, 0, 0);
        if (pid == 0) {
            int code = child(base, scenario);
            if (code != 73)
                say("PREFETCH CHILD FAIL\n");
            call(SYS_EXIT, code, 0, 0);
            return 4;
        }
        int status = -1;
        if (pid < 0 || call(SYS_WAIT, (long)&status, 0, 0) != pid || status != 73) {
            char error[] = "PREFETCH FAIL case 0 code 00\n";
            error[19] += scenario;
            if (status >= 0 && status < 100) {
                error[26] += status / 10;
                error[27] += status % 10;
            }
            say(error);
            return 5;
        }
        for (unsigned i = 0; i < 8; ++i) {
            if (scenario == HOLE && i == 3)
                continue;
            if (base[i * PAGE] != i + 1 || base[i * PAGE + PAGE - 1] != i + 91)
                return 6;
        }
        if (scenario == HOLE) {
            if (call(SYS_MUNMAP, address, 3, 0) || call(SYS_MUNMAP, address + 4 * PAGE, 4, 0))
                return 7;
        } else if (call(SYS_MUNMAP, address, 8, 0)) {
            return 7;
        }
        if (PROBE(PROBE_SNAPSHOT, &after) || !balances(&before, &after))
            return 8;
        const char *names[] = {"EARLY demand=2 prefetch=4 unused=4", "HOLE demand=2 prefetch=1",
                               "WINDOW demand=2 prefetch=0", "OOM demand=2 prefetch=1",
                               "WRITE BOUNDARY demand=2 prefetch=0"};
        say("PREFETCH CASE ");
        say(names[scenario]);
        say("\n");
    }
    say("PREFETCH PASS\n");
    return 0;
}

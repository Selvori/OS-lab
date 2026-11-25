#include "bounded_guest.h"

#define MIXED_KEEP 0x3000UL
#define MIXED_DROP 0x4000UL
#define SHARED_KEEP 0x200000UL
#define SHARED_DROP 0x400000UL

static bool gate(void)
{
    char c = 0;
    return read(0, &c, 1) == 1 && c == 'g';
}

static bool ready(const char *path)
{
    for (unsigned tick = 0; tick < 500; ++tick) {
        if (peek(path)) return true;
        call(SYS_SLEEP, 1, 0, 0);
    }
    return false;
}

/* Both addresses remain borrowed. One is in the mixed private first leaf
 * table, the other in a wholly shared read-only Sv39 leaf table. */
__attribute__((noinline, noclone))
static bool read_held(unsigned count)
{
    unsigned bad = 0;
    for (unsigned i = 0; i < count; ++i) {
        bad |= *(volatile unsigned char *)MIXED_KEEP ^ 0x37U;
        bad |= *(volatile unsigned char *)SHARED_KEEP ^ 0x96U;
    }
    return bad == 0;
}

static int branch(void)
{
    long pid = call(SYS_GETPID, 0, 0, 0);
    CHECK(pid > 0 && *(volatile unsigned char *)MIXED_DROP == 0xa2 &&
          *(volatile unsigned char *)SHARED_DROP == 0x4b);
    CHECK(UNMAP(MIXED_DROP, 1) == 0 && UNMAP(SHARED_DROP, 1) == 0);
    CHECK(publish((pid & 1) ? "/h-odd" : "/h-even", 1));
    for (;;) {
        CHECK(read_held(30000000U));
        if (peek("/h-finish")) break;
    }
    return 51 + (int)(pid & 1);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 && (equal(argv[1], "1") || equal(argv[1], "9")));
    unsigned policy = (unsigned)(argv[1][0] - '0');
    CHECK(MAP(MIXED_KEEP, 2) == MIXED_KEEP && MAP(SHARED_KEEP, 1) == SHARED_KEEP &&
          MAP(SHARED_DROP, 1) == SHARED_DROP);
    *(volatile unsigned char *)MIXED_KEEP = 0x37;
    *(volatile unsigned char *)MIXED_DROP = 0xa2;
    *(volatile unsigned char *)SHARED_KEEP = 0x96;
    *(volatile unsigned char *)SHARED_DROP = 0x4b;

    long frozen = call(SYS_FREEZE, policy, 0, 0);
    if (!frozen) return branch();
    CHECK(frozen == 1);
    long a = call(SYS_SPAWN, 0, 0, 0);
    long b = call(SYS_SPAWN, 0, 0, 0);
    CHECK(a > 0 && b > 0 && ((a ^ b) & 1));
    CHECK(ready("/h-odd") && ready("/h-even"));
    ROW("FAMILY", call(SYS_GETPID, 0, 0, 0), a, b);
    say("HELD OPEN\n");
    CHECK(gate());
    CHECK(UNMAP(MIXED_KEEP, 2) == 0 && UNMAP(SHARED_KEEP, 1) == 0 &&
          UNMAP(SHARED_DROP, 1) == 0);
    /* Let a dispatched sibling enter U before the creator issues close. */
    for (volatile unsigned i = 0; i < 200000U; ++i) {}
    CHECK(call(SYS_RELEASE, 0, 0, 0) == 0 && call(SYS_SPAWN, 0, 0, 0) == -22);
    CHECK(call(SYS_SLEEP, 8, 0, 0) == 0);
    say("HELD CLOSED\n");
    CHECK(gate() && publish("/h-finish", 1));
    unsigned seen = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int status = -1;
        long pid = call(SYS_WAIT, (long)&status, 0, 0);
        CHECK((pid == a || pid == b) && status == 51 + (int)(pid & 1));
        seen |= 1U << (pid == a ? 0 : 1);
    }
    CHECK(seen == 3 && call(SYS_WAIT, 0, 0, 0) == -10);
    CHECK(call(SYS_SLEEP, 8, 0, 0) == 0);
    say("HELD FINAL\n");
    CHECK(gate());
    say("HELD PASS\n");
    return 0;
}

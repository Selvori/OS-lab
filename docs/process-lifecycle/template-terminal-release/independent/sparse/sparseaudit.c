#include "lib.h"

#define BASE 0x3000L
#define PAGE 4096L
#define CHECK(expr) do { if (!(expr)) { say("AUDIT FAIL line\n"); return 1; } } while (0)

struct span { unsigned first, pages; bool keep; };
static const struct span spans[] = {
    {0, 3, true}, {58, 10, false}, {126, 2, true},
    {508, 6, false}, {1020, 4, true}, {4095, 1, false},
};

static unsigned char signature(unsigned i)
{
    return 0x40 + (i * 17 % 61);
}

static bool checkpoint(const char *label)
{
    char c = 0;
    say(label);
    say("\n");
    return read(0, &c, 1) == 1 && c == 'g';
}

static void print_pid(void)
{
    unsigned pid = (unsigned)call(SYS_GETPID, 0, 0, 0);
    char digits[12];
    unsigned pos = sizeof(digits);
    do {
        digits[--pos] = '0' + pid % 10;
        pid /= 10;
    } while (pid);
    say("AUDIT PID ");
    write(1, digits + pos, sizeof(digits) - pos);
    say("\n");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    unsigned policy = (unsigned)(argv[1][0] - '0');
    volatile unsigned char *heap = (void *)BASE;
    unsigned count = sizeof(spans) / sizeof(spans[0]);
    for (unsigned k = 0; k < count; ++k) {
        const struct span *r = &spans[k];
        long va = BASE + r->first * PAGE;
        CHECK(call(SYS_MMAP, va, r->pages, 0) == va);
        for (unsigned i = r->first; i < r->first + r->pages; ++i)
            heap[i * PAGE] = signature(i);
    }
    print_pid();
    CHECK(checkpoint("AUDIT ORIGIN"));
    long rc = call(SYS_FREEZE, policy, 0, 0);
    if (rc == 0) {
        for (unsigned k = 0; k < count; ++k)
            for (unsigned i = spans[k].first; i < spans[k].first + spans[k].pages; ++i)
                CHECK(heap[i * PAGE] == signature(i));
        return 73;
    }
    CHECK(rc == 1);
    long child = call(SYS_SPAWN, 0, 0, 0);
    int status = -1;
    CHECK(child > 0 && call(SYS_WAIT, (long)&status, 0, 0) == child && status == 73);
    for (unsigned k = 0; k < count; ++k)
        if (!spans[k].keep)
            CHECK(call(SYS_MUNMAP, BASE + spans[k].first * PAGE, spans[k].pages, 0) == 0);
    CHECK(checkpoint("AUDIT BEFORE CLOSE"));
    CHECK(call(SYS_RELEASE, 0, 0, 0) == 0);
    for (unsigned i = 0; i < 128; ++i)
        call(SYS_YIELD, 0, 0, 0);
    for (unsigned k = 0; k < count; ++k)
        if (spans[k].keep)
            for (unsigned i = spans[k].first; i < spans[k].first + spans[k].pages; ++i)
                CHECK(heap[i * PAGE] == signature(i));
    CHECK(checkpoint("AUDIT DRAINED"));
    for (unsigned k = 0; k < count; ++k)
        if (spans[k].keep)
            CHECK(call(SYS_MUNMAP, BASE + spans[k].first * PAGE, spans[k].pages, 0) == 0);
    CHECK(checkpoint("AUDIT FINAL"));
    say("TERMINAL AUDIT PASS\n");
    return 0;
}

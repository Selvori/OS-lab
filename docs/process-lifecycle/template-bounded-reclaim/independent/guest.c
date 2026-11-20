#include "lib.h"

#define PAGE 4096UL
#define BASE 0x3000UL

static int fail(int line)
{
    char digits[16];
    unsigned at = sizeof(digits);
    unsigned number = (unsigned)line;
    do {
        digits[--at] = '0' + number % 10;
        number /= 10;
    } while (number);
    say("AUDIT FAIL line ");
    write(1, digits + at, sizeof(digits) - at);
    say("\n");
    return 1;
}
#define REQUIRE(test) do { if (!(test)) return fail(__LINE__); } while (0)

static void announce(const char *label, int a, int b, int c)
{
    const int values[] = {a, b, c};
    say(label);
    for (unsigned j = 0; j < 3; ++j) {
        char buf[16];
        unsigned at = sizeof(buf), n = (unsigned)values[j];
        do { buf[--at] = '0' + n % 10; n /= 10; } while (n);
        say(" ");
        write(1, buf + at, sizeof(buf) - at);
    }
    say("\n");
}

static bool checkpoint(const char *name)
{
    char input = 0;
    say(name);
    say("\n");
    return read(0, &input, 1) == 1 && input == 'g';
}

static bool publish(const char *path, int value)
{
    long fd = open(path, OPEN_CREATE | OPEN_WRONLY);
    if (fd < 0) return false;
    long rc = write(fd, &value, sizeof(value));
    close(fd);
    return rc == sizeof(value);
}

static int receive(const char *path)
{
    for (;;) {
        long fd = open(path, OPEN_RDONLY);
        if (fd >= 0) {
            int value = 0;
            long size = read(fd, &value, sizeof(value));
            close(fd);
            if (size == sizeof(value)) return value;
        }
        call(SYS_YIELD, 0, 0, 0);
    }
}

static bool pinned(unsigned k, unsigned role)
{
    const unsigned starts[] = {0, 509, 1021, 1533, 2045};
    for (unsigned w = 0; w < 5; ++w) {
        if (k == starts[w] + role) return true;
    }
    return false;
}

static unsigned char initial(unsigned k)
{
    return (unsigned char)(k % 197 + 7);
}

static int branch(unsigned pages, bool pin)
{
    int first = receive("/first");
    unsigned role = call(SYS_GETPID, 0, 0, 0) == first ? 0 : 1;
    REQUIRE(receive("/go") == 1);
    volatile unsigned char *heap = (void *)BASE;
    unsigned start = 0;
    for (unsigned k = 0; k <= pages; ++k) {
        if (k < pages && !(pin && pinned(k, role))) continue;
        if (k != start)
            REQUIRE(call(SYS_MUNMAP, BASE + start * PAGE, k - start, 0) == 0);
        if (k < pages) REQUIRE(heap[k * PAGE] == initial(k));
        start = k + 1;
    }
    REQUIRE(publish(role ? "/r1" : "/r0", 1));
    REQUIRE(receive("/done") == 1);
    if (pin) {
        for (unsigned k = 0; k < pages; ++k) {
            if (pinned(k, role)) REQUIRE(heap[k * PAGE] == initial(k));
        }
    }
    return 77;
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 3);
    unsigned policy = (unsigned)(argv[1][0] - '0');
    bool pin = equal(argv[2], "pin");
    REQUIRE(pin || equal(argv[2], "owner"));
    unsigned pages = pin ? 2048 : 1;
    REQUIRE(call(SYS_MMAP, BASE, pages, 0) == BASE);
    volatile unsigned char *heap = (void *)BASE;
    for (unsigned k = 0; k < pages; ++k) heap[k * PAGE] = initial(k);
    int parent = (int)call(SYS_GETPID, 0, 0, 0);
    announce("AUDIT ORIGIN PID", parent, 0, 0);
    REQUIRE(checkpoint("AUDIT ORIGIN"));
    long frozen = call(SYS_FREEZE, policy, 0, 0);
    if (frozen == 0) return branch(pages, pin);
    REQUIRE(frozen == 1);
    int a = (int)call(SYS_SPAWN, 0, 0, 0);
    int b = (int)call(SYS_SPAWN, 0, 0, 0);
    REQUIRE(a > 0 && b > 0 && a != b && publish("/first", a));
    announce("AUDIT FAMILY", parent, a, b);
    REQUIRE(checkpoint("AUDIT ATTACHED"));
    REQUIRE(call(SYS_RELEASE, 0, 0, 0) == 0);
    if (pin) REQUIRE(call(SYS_MUNMAP, BASE, pages, 0) == 0);
    REQUIRE(publish("/go", 1));
    REQUIRE(receive("/r0") == 1 && receive("/r1") == 1);
    for (unsigned i = 0; i < 3072; ++i) call(SYS_YIELD, 0, 0, 0);
    REQUIRE(checkpoint("AUDIT HELD"));
    if (!pin) {
        REQUIRE(heap[0] == initial(0));
        REQUIRE(call(SYS_MUNMAP, BASE, 1, 0) == 0);
        REQUIRE(checkpoint("AUDIT CREATOR RELEASED"));
    }
    REQUIRE(publish("/done", 1));
    for (unsigned n = 0; n < 2; ++n) {
        int status = -1;
        REQUIRE(call(SYS_WAIT, (long)&status, 0, 0) > 0 && status == 77);
    }
    for (unsigned i = 0; i < 3072; ++i) call(SYS_YIELD, 0, 0, 0);
    REQUIRE(checkpoint("AUDIT FINAL"));
    say("INDEPENDENT AUDIT PASS\n");
    return 0;
}

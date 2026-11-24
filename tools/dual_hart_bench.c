#include "lib.h"

static uint64 now(void)
{
    uint64 value;
    __asm__ volatile("rdtime %0" : "=r"(value) :: "memory");
    return value;
}

static char *decimal(char *out, uint64 number)
{
    char reversed[20];
    unsigned length = 0;
    do {
        reversed[length++] = '0' + number % 10;
        number /= 10;
    } while (number);
    while (length) *out++ = reversed[--length];
    return out;
}

/* Keep a symbol and size for the separately sampled GDB privilege witness. */
__attribute__((noinline, noclone))
static uint32 calculate(unsigned iterations)
{
    uint32 value = 1;
    for (unsigned i = 0; i < iterations; ++i)
        value = value * 1664525U + 1013904223U;
    return value & 0x7fffffffU;
}

int main(int argc, char **argv)
{
    unsigned iterations = 0;
    if (argc != 2) return 1;
    for (const char *p = argv[1]; *p; ++p) {
        if (*p < '0' || *p > '9' || iterations > 90000000U) return 1;
        iterations = iterations * 10 + (*p - '0');
    }
    if (!iterations) return 1;

    volatile int cookie = 0;
    say("DUAL START\n");
    uint64 start = now();
    long child = call(SYS_FORK, 0, 0, 0);
    if (child < 0) return 2;
    /* The fork siblings must retain different writes to their COW stack. */
    int expected_cookie = child ? 41 : 73;
    cookie = expected_cookie;
    if (!child && call(SYS_GETPID, 0, 0, 0) <= 0) return -1;
    uint32 checksum = calculate(iterations);
    if (cookie != expected_cookie) return -1;
    if (child == 0) return (int)checksum;

    int status = -1;
    if (call(SYS_WAIT, (long)&status, 0, 0) != child || status != (int)checksum)
        return 3;
    uint64 elapsed = now() - start;
    if (call(SYS_WAIT, 0, 0, 0) != -10) return 4;

    char text[96] = "DUAL PASS ";
    char *out = decimal(text + 10, iterations);
    *out++ = ' ';
    out = decimal(out, checksum);
    *out++ = ' ';
    out = decimal(out, (uint32)status);
    *out++ = ' ';
    out = decimal(out, elapsed);
    *out++ = '\n';
    *out = '\0';
    say(text);
    return 0;
}

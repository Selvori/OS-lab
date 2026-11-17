#include "lib.h"
#include <kernel/lifecycle_probe.h>

#define PAGE 4096UL
#define MAX_RUNS 16

struct round_sample {
    unsigned long elapsed;
    struct lifecycle_sample kernel;
};

_Static_assert(sizeof(struct round_sample) * MAX_RUNS <= PAGE, "sample storage");

static unsigned long now(void)
{
    unsigned long value;
    __asm__ volatile("rdtime %0" : "=r"(value) :: "memory");
    return value;
}

static unsigned number(const char *text)
{
    unsigned n = 0;
    while (*text >= '0' && *text <= '9')
        n = n * 10 + *text++ - '0';
    return *text ? 0 : n;
}

static void print_number(unsigned long n)
{
    char text[22];
    size_t pos = sizeof(text);
    do {
        text[--pos] = '0' + n % 10;
        n /= 10;
    } while (n);
    text[--pos] = ' ';
    write(1, text + pos, sizeof(text) - pos);
}

static bool same_resources(const struct lifecycle_sample *a,
                           const struct lifecycle_sample *b)
{
    return a->user_left == b->user_left && a->kernel_left == b->kernel_left &&
           a->nodes_left == b->nodes_left && a->slots_used == b->slots_used &&
           a->files_used == b->files_used;
}

static unsigned char pattern(unsigned page)
{
    return (unsigned char)(page * 17 + 3);
}

/* The child visits each page it modifies. The read-only job checks every page. */
static int work(volatile unsigned char *heap, unsigned pages,
                unsigned writes, unsigned stride)
{
    unsigned count = writes ? writes : pages;
    for (unsigned i = 0; i < count; ++i) {
        unsigned page = writes ? i * stride : i;
        volatile unsigned char *byte = heap + page * PAGE;
        if (*byte != pattern(page) || byte[PAGE - 1] != pattern(page))
            return 1;
        if (writes) {
            *byte ^= 255;
            if (*byte == pattern(page))
                return 2;
        }
    }
    return 73;
}

int main(int argc, char **argv)
{
    if (argc != 6)
        return 1;
    unsigned pages = number(argv[1]), writes = number(argv[2]);
    unsigned stride = number(argv[3]), runs = number(argv[4]);
    bool exec = equal(argv[5], "exec");
    if (pages > 4096 || !stride || runs > MAX_RUNS || !runs ||
        (writes && (writes - 1) * stride >= pages))
        return 2;

    unsigned long empty[16];
    for (unsigned i = 0; i < 16; ++i) {
        unsigned long before = now();
        empty[i] = now() - before;
    }
    struct lifecycle_sample original, ready, final;
    if (call(SYS_LIFECYCLE_PROBE, PROBE_SNAPSHOT, (long)&original, 0))
        return 3;
    unsigned long started = now();
    long address = pages ? call(SYS_MMAP, 0, pages, 0) : 0;
    if (address < 0)
        return 4;
    volatile unsigned char *heap = (void *)address;
    for (unsigned i = 0; i < pages; ++i)
        for (unsigned j = 0; j < PAGE; ++j)
            heap[i * PAGE + j] = pattern(i);
    unsigned long init_time = now() - started;
    if (call(SYS_LIFECYCLE_PROBE, PROBE_SNAPSHOT, (long)&ready, 0))
        return 5;

    struct round_sample *records = (void *)0x2000;
    started = now();
    for (unsigned i = 0; i < runs; ++i) {
        if (call(SYS_LIFECYCLE_PROBE, PROBE_START, 0, 0))
            return 6;
        unsigned long before = now();
        long pid = call(SYS_FORK, 0, 0, 0);
        if (pid == 0) {
            int status;
            if (exec) {
                const char *args[] = {"/bin/lifejob", "work", 0};
                call(SYS_EXEC, (long)args[0], (long)args, 0);
                status = 3;
            } else {
                status = work(heap, pages, writes, stride);
            }
            call(SYS_EXIT, status, 0, 0);
            return 7;
        }
        int status;
        if (pid < 0 || call(SYS_WAIT, (long)&status, 0, 0) != pid || status != 73)
            return 8;
        unsigned long elapsed = now() - before;
        struct lifecycle_sample sample;
        if (call(SYS_LIFECYCLE_PROBE, PROBE_STOP, (long)&sample, 0) ||
            !same_resources(&sample, &ready))
            return 9;
        records[i] = (struct round_sample){elapsed, sample};
        if (work(heap, pages, 0, 1) != 73)
            return 10;
    }
    unsigned long batch_time = now() - started;
    if ((pages && call(SYS_MUNMAP, address, pages, 0)) ||
        call(SYS_LIFECYCLE_PROBE, PROBE_SNAPSHOT, (long)&final, 0) ||
        !same_resources(&original, &final))
        return 11;

    say("LIFE BATCH");
    print_number(init_time);
    print_number(batch_time);
    for (unsigned i = 0; i < 16; ++i)
        print_number(empty[i]);
    say("\n");
    for (unsigned i = 0; i < runs; ++i) {
        say("LIFE ROW");
        print_number(records[i].elapsed);
        unsigned long fields[sizeof(struct lifecycle_sample) / sizeof(unsigned long)];
        __builtin_memcpy(fields, &records[i].kernel, sizeof(fields));
        for (unsigned j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j)
            print_number(fields[j]);
        say("\n");
    }
    say("LIFE PASS\n");
    return 0;
}

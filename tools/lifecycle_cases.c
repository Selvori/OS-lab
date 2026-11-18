#include "lib.h"
#include <kernel/lifecycle_probe.h>

#define PAGE 4096
#define W (1UL << 2)
#define COW (1UL << 8)

static void check(bool ok, unsigned line)
{
    if (ok)
        return;
    char text[] = "LIFE CHECK FAIL 0000\n";
    for (int i = 19; i >= 16; --i) {
        text[i] = '0' + line % 10;
        line /= 10;
    }
    say(text);
    call(SYS_EXIT, 1, 0, 0);
    for (;;) {}
}

#define CHECK(x) check((x), __LINE__)

static long fork_process(void) { return call(SYS_FORK, 0, 0, 0); }
static long wait_for(void *status) { return call(SYS_WAIT, (long)status, 0, 0); }
static void pause_for(unsigned ticks) { call(SYS_SLEEP, ticks, 0, 0); }

static void finish(int status) __attribute__((noreturn));
static void finish(int status)
{
    call(SYS_EXIT, status, 0, 0);
    for (;;) {}
}

static struct lifecycle_mapping mapping(void *address)
{
    struct lifecycle_mapping m = { .address = (unsigned long)address };
    CHECK(call(SYS_LIFECYCLE_PROBE, PROBE_MAPPING, (long)&m, 0) == 0);
    return m;
}

static bool same_mapping(struct lifecycle_mapping a, struct lifecycle_mapping b)
{
    return a.frame == b.frame && a.flags == b.flags && a.refs == b.refs;
}

static void resources(struct lifecycle_sample *sample)
{
    CHECK(call(SYS_LIFECYCLE_PROBE, PROBE_SNAPSHOT, (long)sample, 0) == 0);
}

static void check_resources(const struct lifecycle_sample *before)
{
    struct lifecycle_sample after;
    resources(&after);
    CHECK(before->user_left == after.user_left && before->kernel_left == after.kernel_left &&
          before->nodes_left == after.nodes_left && before->slots_used == after.slots_used &&
          before->files_used == after.files_used);
}

static unsigned char *heap_pages(unsigned pages)
{
    long start = call(SYS_MMAP, 0, pages, 0);
    CHECK(start == 0x3000);
    return (void *)start;
}

static void unmap(void *start, unsigned pages)
{
    CHECK(call(SYS_MUNMAP, (long)start, pages, 0) == 0);
}

static void tree(void)
{
    unsigned char *heap = heap_pages(3);
    *heap = 11;
    heap[2 * PAGE] = 13;
    struct lifecycle_mapping fixed = mapping(heap);
    CHECK((fixed.flags & (W | COW)) == W && fixed.refs == 1);
    long pids[2];
    for (unsigned i = 0; i < 2; ++i) {
        long pid = fork_process();
        CHECK(pid >= 0);
        if (!pid) {
            struct lifecycle_mapping shared = mapping(heap);
            CHECK(shared.frame == fixed.frame && shared.refs >= 2 &&
                  (shared.flags & (W | COW)) == COW && *heap == 11);
            *heap = 21 + i;
            struct lifecycle_mapping copy = mapping(heap);
            CHECK(copy.frame != fixed.frame && copy.refs == 1 && (copy.flags & (W | COW)) == W);
            if (i == 0) {
                long grand = fork_process();
                CHECK(grand >= 0);
                if (!grand) {
                    CHECK(*heap == 21);
                    *heap = 31;
                    CHECK(*heap == 31);
                    unmap(heap + 2 * PAGE, 1);
                    finish(61);
                }
                int status;
                CHECK(wait_for(&status) == grand && status == 61);
                CHECK(*heap == 21 && heap[2 * PAGE] == 13);
            }
            finish(41 + i);
        }
        pids[i] = pid;
    }
    unsigned seen = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int status = 0;
        long pid = wait_for(&status);
        CHECK((pid == pids[0] && status == 41) || (pid == pids[1] && status == 42));
        seen |= 1U << (status - 41);
    }
    CHECK(seen == 3 && *heap == 11 && heap[2 * PAGE] == 13);
    CHECK(mapping(heap).refs == 1);
    *heap = 12;
    struct lifecycle_mapping sole = mapping(heap);
    CHECK(sole.frame == fixed.frame && (sole.flags & (W | COW)) == W && sole.refs == 1);
    unmap(heap, 3);
    say("LIFE CASE tree, first split, sole upgrade, partial unmap\n");
}

static void copyout(void)
{
    unsigned char *heap = heap_pages(2);
    unsigned char *cross = heap + PAGE - 2;
    for (unsigned i = 0; i < 4; ++i)
        cross[i] = 0x66;
    long pid = fork_process();
    CHECK(pid >= 0);
    if (!pid)
        finish(0x11223344);
    pause_for(2);
    CHECK(wait_for((void *)0x1000) == -14);
    struct lifecycle_mapping a = mapping(cross), b = mapping(cross + 2);
    CHECK(a.refs == 2 && b.refs == 2 && (a.flags & COW) && (b.flags & COW));
    CHECK(call(SYS_LIFECYCLE_PROBE, PROBE_FAIL_AFTER, 1, 0) == 0);
    CHECK(wait_for(cross) == -12);
    CHECK(same_mapping(a, mapping(cross)) && same_mapping(b, mapping(cross + 2)));
    for (unsigned i = 0; i < 4; ++i)
        CHECK(cross[i] == 0x66);
    CHECK(wait_for(cross) == pid && cross[0] == 0x44 && cross[1] == 0x33 &&
          cross[2] == 0x22 && cross[3] == 0x11);
    CHECK(wait_for(0) == -10);
    CHECK(mapping(cross).frame != a.frame && mapping(cross + 2).frame != b.frame);

    long fd = open("/etc/motd", OPEN_RDONLY);
    CHECK(fd >= 0);
    pid = fork_process();
    CHECK(pid >= 0);
    if (!pid) {
        pause_for(5);
        CHECK(cross[0] == 0x44 && cross[1] == 0x33 && cross[2] == 0x22 && cross[3] == 0x11);
        finish(43);
    }
    CHECK(read(fd, heap + 2 * PAGE - 2, 4) == -14);
    a = mapping(cross);
    b = mapping(cross + 2);
    CHECK(call(SYS_LIFECYCLE_PROBE, PROBE_FAIL_AFTER, 1, 0) == 0);
    CHECK(read(fd, cross, 4) == -12);
    CHECK(same_mapping(a, mapping(cross)) && same_mapping(b, mapping(cross + 2)));
    CHECK(cross[0] == 0x44 && cross[1] == 0x33 && cross[2] == 0x22 && cross[3] == 0x11);
    CHECK(read(fd, cross, 4) == 4 && cross[0] == 'W' && cross[1] == 'e' &&
          cross[2] == 'l' && cross[3] == 'c');
    int status;
    CHECK(wait_for(&status) == pid && status == 43);
    close(fd);
    unmap(heap, 2);
    say("LIFE CASE cross-page wait/read, bad address, second allocation rollback\n");
}

static void faults_and_exec(void)
{
    unsigned char *heap = heap_pages(1);
    *heap = 71;
    long pid = fork_process();
    CHECK(pid >= 0);
    if (!pid) {
        CHECK(call(SYS_LIFECYCLE_PROBE, PROBE_FAIL_AFTER, 0, 0) == 0);
        /* No intervening call can allocate between arming and the store. */
        *(volatile unsigned char *)heap = 72;
        finish(7);
    }
    int status;
    CHECK(wait_for(&status) == pid && status == -1 && *heap == 71);
    pid = fork_process();
    CHECK(pid >= 0);
    if (!pid) {
        const char *args[] = {"/bin/lifejob", "work", 0};
        const unsigned long *rx = (void *)0x1000;
        unsigned long first_instruction = *rx;
        for (unsigned i = 0; i < 3; ++i) {
            CHECK(call(SYS_LIFECYCLE_PROBE, PROBE_FAIL_AFTER, i, 0) == 0);
            CHECK(call(SYS_EXEC, (long)args[0], (long)args, 0) == -12);
            CHECK(*heap == 71 && *rx == first_instruction);
        }
        CHECK(call(SYS_EXEC, (long)args[0], (long)args, 0) < 0);
        finish(1);
    }
    CHECK(wait_for(&status) == pid && status == 73 && *heap == 71);
    pid = fork_process();
    CHECK(pid >= 0);
    if (!pid) {
        *(volatile unsigned char *)0x1000 = 0;
        finish(8);
    }
    CHECK(wait_for(&status) == pid && status == -1 && *heap == 71);
    unmap(heap, 1);
    say("LIFE CASE store OOM, three exec allocation failures, RX protection\n");
}

static void sparse_cross_table(void)
{
    const unsigned long addresses[] = {0x1ff000, 0x200000, 0x401000, 0x1002000};
    unsigned n = sizeof(addresses) / sizeof(addresses[0]);
    unsigned long frames[4];
    for (unsigned i = 0; i < n; ++i) {
        CHECK(call(SYS_MMAP, addresses[i], 1, 0) == (long)addresses[i]);
        *(volatile unsigned char *)addresses[i] = 41 + i;
        frames[i] = mapping((void *)addresses[i]).frame;
    }

    struct lifecycle_sample mapped;
    resources(&mapped);
    /* The third L0 allocation fails after the unpublished child mapped
     * heap leaves in the first two tables. */
    CHECK(call(SYS_LIFECYCLE_PROBE, PROBE_FAIL_AFTER, 8, 0) == 0);
    CHECK(fork_process() == -12);
    check_resources(&mapped);
    for (unsigned i = 0; i < n; ++i) {
        struct lifecycle_mapping m = mapping((void *)addresses[i]);
        CHECK(m.frame == frames[i] && m.refs == 1 && (m.flags & (W | COW)) == W);
    }

    long pid = fork_process();
    CHECK(pid >= 0);
    if (!pid) {
        CHECK(call(SYS_MUNMAP, 0x200000, 2, 0) == -22);
        CHECK(*(volatile unsigned char *)0x200000 == 42);
        *(volatile unsigned char *)0x200000 = 99;
        finish(53);
    }
    int status;
    CHECK(wait_for(&status) == pid && status == 53);
    check_resources(&mapped);
    for (unsigned i = 0; i < n; ++i) {
        void *address = (void *)addresses[i];
        struct lifecycle_mapping m = mapping(address);
        CHECK(m.frame == frames[i] && m.refs == 1 && *(volatile unsigned char *)address == 41 + i);
        unmap(address, 1);
    }
    say("LIFE CASE sparse cross-table, failed fork, shared frames\n");
}

int main(void)
{
    struct lifecycle_sample before;
    resources(&before);
    tree();
    check_resources(&before);
    copyout();
    check_resources(&before);
    faults_and_exec();
    check_resources(&before);
    sparse_cross_table();
    check_resources(&before);
    say("LIFE CHECK PASS\n");
    return 0;
}

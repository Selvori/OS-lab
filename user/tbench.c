#include "lib.h"
#include <kernel/lifecycle_probe.h>

static unsigned num(const char *s)
{
    unsigned n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0';
    return n;
}
static unsigned long now(void)
{
    unsigned long t;
    __asm__ volatile("rdtime %0" : "=r"(t) :: "memory");
    return t;
}
static void number(unsigned long x)
{
    char buf[22];
    unsigned i = sizeof(buf);
    do { buf[--i] = '0' + x % 10; x /= 10; } while (x);
    buf[--i] = ' ';
    write(1, buf+i, sizeof(buf)-i);
}
static int job(unsigned pages, unsigned writes, unsigned stride, char kind,
               volatile unsigned char *heap)
{
    if (kind == 'e') {
        const char *args[] = {"/bin/lifejob", "work", 0};
        call(SYS_EXEC, (long)args[0], (long)args, 0);
        return 2;
    }
    if (kind == 'q') {
        unsigned hash = 0, expected = 0;
        for (unsigned i = 0; i < 4096; ++i) {
            unsigned k = (i * 137 + 53) % pages;
            hash = hash * 33 + heap[k * 4096];
            expected = expected * 33 + (unsigned char)(k * 17 + 3);
        }
        return hash == expected ? 73 : 3;
    }
    if (kind == 'u') return call(SYS_MUNMAP,(long)heap+508*4096,3,0) ? 4 : 73;
    for (unsigned i = 0; i < (writes ? writes : pages); ++i) {
        unsigned k = writes ? i*stride : i;
        if (heap[k*4096] != (unsigned char)(k*17+3)) return 5;
        if (writes) heap[k*4096] ^= 255;
    }
    return 73;
}
int main(int argc, char **argv)
{
    if (argc != 7) return 1;
    unsigned policy=num(argv[1]), pages=num(argv[2]), writes=num(argv[3]);
    unsigned stride=num(argv[4]), n=num(argv[5]);
    char kind=argv[6][0];
    if (!pages || pages>4096 || !n || n>64 || policy>3 ||
        (writes && (writes-1)*stride >= pages)) return 2;
    struct lifecycle_sample before, after, sample;
    struct template_stats stats;
    call(SYS_LIFECYCLE_PROBE,PROBE_SNAPSHOT,(long)&before,0);
    long addr=call(SYS_MMAP,kind=='a' ? 0x200000 : 0,pages,0);
    if (addr<0 || (kind=='a' && addr!=0x200000)) return 3;
    volatile unsigned char *heap=(void *)addr;
    for (unsigned i=0;i<pages;++i) heap[i*4096]=(unsigned char)(i*17+3);
    unsigned long *rounds=(void *)0x2000;
    call(SYS_LIFECYCLE_PROBE,PROBE_START,0,0);
    unsigned long start=now();
    long frozen=policy ? call(SYS_FREEZE,policy-1,0,0) : 1;
    if (!frozen) return job(pages,writes,stride,kind,heap);
    if (frozen!=1) return 4;
    unsigned long setup=now()-start, spawn_ticks=0;
    for (unsigned i=0;i<n;++i) {
        unsigned long t=now();
        long pid=call(policy ? SYS_SPAWN : SYS_FORK,0,0,0);
        if (!pid) return job(pages,writes,stride,kind,heap);
        spawn_ticks+=now()-t;
        int status;
        if (pid<0 || call(SYS_WAIT,(long)&status,0,0)!=pid || status!=73) return 5;
        rounds[i]=now()-t;
    }
    unsigned long t=now();
    if (policy && call(SYS_RELEASE,0,0,0)) return 6;
    unsigned long end=now();
    call(SYS_LIFECYCLE_PROBE,PROBE_STOP,(long)&sample,0);
    call(SYS_LIFECYCLE_PROBE,PROBE_TEMPLATE,(long)&stats,0);
    for (unsigned i=0;i<pages;++i) if (heap[i*4096]!=(unsigned char)(i*17+3)) return 7;
    if (call(SYS_MUNMAP,addr,pages,0)) return 8;
    call(SYS_LIFECYCLE_PROBE,PROBE_SNAPSHOT,(long)&after,0);
    if (before.user_left!=after.user_left || before.kernel_left!=after.kernel_left ||
        before.slots_used!=after.slots_used || before.nodes_left!=after.nodes_left ||
        before.files_used!=after.files_used) return 9;
    say("TB"); number(end-start); number(setup); number(end-t); number(spawn_ticks);
    number(sample.reap_time); number(sample.user_peak); number(sample.kernel_peak);
    number(stats.copied_slots); number(stats.retains); number(stats.exit_slots);
    number(stats.bitmap_words); number(stats.private_drops); number(stats.resident_page_ticks);
    number(stats.first_write_ticks); number(stats.first_writes);
    for (unsigned i=0;i<n;++i) number(rounds[i]);
    say("\n");
    return 0;
}

/* Sample a live branch with no creator heap references, using the tpin handshake. */
#include "lib.h"
#include <kernel/lifecycle_probe.h>

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
#define PROBE(op,arg) call(SYS_LIFECYCLE_PROBE,op,(long)(arg),0)
#define PAGES 2048
#define PAGE 4096

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

static bool equal_resources(const struct lifecycle_sample *a, const struct lifecycle_sample *b)
{
    return a->user_left==b->user_left && a->kernel_left==b->kernel_left &&
           a->slots_used==b->slots_used && a->nodes_left==b->nodes_left &&
           a->files_used==b->files_used;
}

static void live(const struct lifecycle_sample *before, unsigned covered)
{
    struct lifecycle_sample pools;
    struct template_stats stats;
    PROBE(PROBE_BALANCE,&pools);
    PROBE(PROBE_TEMPLATE,&stats);
    say("WP"); number(covered);
    number(before->user_left-pools.user_left);
    number(before->kernel_left-pools.kernel_left);
    number(stats.released_windows); number(stats.dropped_refs); say("\n");
}

static int cover(volatile unsigned char *heap, const char *marker,
                 const struct lifecycle_sample *before)
{
    long fd;
    while ((fd=open(marker,OPEN_RDONLY))<0) call(SYS_YIELD,0,0,0);
    close(fd);
    const unsigned limits[]={0,508,509,2047,2048};
    const unsigned last[]={508,1020,1532,2044,2047};
    unsigned current=0, window=0;
    unsigned long release[5];
    for (unsigned stage=0;stage<5;++stage) {
        for (;current<limits[stage];++current) {
            CHECK(heap[current*PAGE]==(unsigned char)(current*17+3));
            if (window<5 && current==last[window]) {
                unsigned long t=now();
                heap[current*PAGE]^=255;
                release[window++]=now()-t;
            } else {
                heap[current*PAGE]^=255;
            }
        }
        live(before,current);
    }
    say("WE");
    for (unsigned w=0;w<5;++w) number(release[w]);
    say("\n");
    call(SYS_SLEEP,5,0,0);
    for (unsigned i=0;i<PAGES;++i)
        CHECK(heap[i*PAGE]==(unsigned char)((i*17+3)^255));
    return 73;
}

int main(int argc, char **argv)
{
    CHECK(argc==2 && (argv[1][0]=='1' || argv[1][0]=='2'));
    unsigned policy=argv[1][0]-'0';
    const char *marker="/window-pin";
    struct lifecycle_sample before, after;
    struct template_stats stats;
    CHECK(!PROBE(PROBE_SNAPSHOT,&before));
    CHECK(!PROBE(PROBE_START,0));
    long addr=call(SYS_MMAP,0,PAGES,0);
    CHECK(addr==0x3000);
    volatile unsigned char *heap=(void *)addr;
    for (unsigned i=0;i<PAGES;++i) heap[i*PAGE]=i*17+3;
    long rc=call(SYS_FREEZE,policy,0,0);
    if (!rc) return cover(heap,marker,&before);
    CHECK(rc==1 && call(SYS_SPAWN,0,0,0)>0);
    unsigned long start=now();
    CHECK(!call(SYS_RELEASE,0,0,0));
    unsigned long closed=now()-start;
    CHECK(!call(SYS_MUNMAP,addr,PAGES,0));
    long fd=open(marker,OPEN_CREATE|OPEN_WRONLY);
    CHECK(fd>=0);
    close(fd);
    int status;
    CHECK(call(SYS_WAIT,(long)&status,0,0)>0 && status==73);
    CHECK(!call(SYS_UNLINK,(long)marker,0,0));
    CHECK(!PROBE(PROBE_STOP,&after) && equal_resources(&before,&after));
    CHECK(!PROBE(PROBE_TEMPLATE,&stats));
    say("WT"); number(closed);
    number(stats.close_scan_branches); number(stats.close_scan_slots);
    number(stats.release_ticks); number(stats.max_release_ticks);
    number(after.user_peak); number(after.kernel_peak);
    number(before.user_left); number(after.user_left);
    number(before.kernel_left); number(after.kernel_left);
    number(before.nodes_left); number(after.nodes_left);
    number(before.slots_used); number(after.slots_used);
    number(before.files_used); number(after.files_used);
    say("\nWP PASS\n");
    return 0;
}

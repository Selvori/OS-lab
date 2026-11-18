#include "lib.h"
#include <kernel/lifecycle_probe.h>

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
#define PROBE(op,arg) call(SYS_LIFECYCLE_PROBE,op,(long)(arg),0)
#define SPAWN() call(SYS_SPAWN,0,0,0)
#define RELEASE() call(SYS_RELEASE,0,0,0)
static bool equal_resources(struct lifecycle_sample *a, struct lifecycle_sample *b)
{
    return a->kernel_left==b->kernel_left && a->user_left==b->user_left &&
           a->slots_used==b->slots_used && a->nodes_left==b->nodes_left &&
           a->files_used==b->files_used;
}

static bool release_counts(unsigned windows, unsigned refs)
{
    struct template_stats s;
    return !PROBE(PROBE_TEMPLATE,&s) && s.released_windows==windows && s.dropped_refs==refs;
}

static int check_policy(unsigned policy, unsigned mode)
{
    struct lifecycle_sample original, stable, after;
    CHECK(!PROBE(PROBE_SNAPSHOT,&original));
    unsigned pages = mode==4 ? 4096 : 2;
    long base = mode==4 ? 0x3000 : 0x3ff000;
    CHECK(call(SYS_MMAP,base,pages,0)==base);
    volatile unsigned char *p=(void *)base;
    p[0]=19;
    p[4096]=27;
    if (mode==0) {
        CHECK(!PROBE(PROBE_SNAPSHOT,&stable));
        for (unsigned i=0;i<8;++i) {
            CHECK(!PROBE(PROBE_FAIL_AFTER,i));
            CHECK(call(SYS_FREEZE,policy,0,0)==-12);
            CHECK(!PROBE(PROBE_SNAPSHOT,&after) && equal_resources(&stable,&after));
        }
    }
    long rc=call(SYS_FREEZE,policy,0,0);
    if (!rc) {
        CHECK(call(SYS_FORK,0,0,0)==-95);
        if (mode==1) {
            *(volatile unsigned char *)0x1000=3;
            return 7;
        }
        if (mode==2) {
            CHECK(!PROBE(PROBE_FAIL_AFTER,0));
            p[0]=8;
            return 8;
        }
        if (mode==4) {
            for (unsigned i=0;i<pages;++i) p[i*4096]=99;
            return 73;
        }
        if (mode==3) {
            const char *args[]={"/bin/lifejob","work",0};
            for (unsigned i=0;i<3;++i) {
                CHECK(!PROBE(PROBE_FAIL_AFTER,i));
                CHECK(call(SYS_EXEC,(long)args[0],(long)args,0)==-12);
                CHECK(*p==19 && p[4096]==27);
            }
            return call(SYS_EXEC,(long)args[0],(long)args,0);
        }
        /* The owner can close its handle and remove its original mappings
         * while eight branches still refer to the frozen image. */
        call(SYS_SLEEP,4,0,0);
        CHECK(*p==19 && p[4096]==27);
        long fd=open("/etc/motd",OPEN_RDONLY);
        CHECK(fd>=0);
        void *cross=(void *)(base+4094);
        /* On an already-private stack, a failing second leaf allocation
         * must leave both windows and every free-list balance unchanged. */
        unsigned sample_op=mode==5 && policy==2 ? PROBE_BALANCE : PROBE_SNAPSHOT;
        if (mode==5) {
            /* Balance probes capture values before writing to the user stack. */
            CHECK(!PROBE(sample_op,&stable));
            if (policy==2) CHECK(release_counts(0,0));
            CHECK(!PROBE(sample_op,&stable));
            for (unsigned i=0;i<2;++i) {
                CHECK(!PROBE(PROBE_FAIL_AFTER,i));
                CHECK(call(SYS_MUNMAP,base,2,0)==-12);
                CHECK(*p==19 && p[4096]==27);
                CHECK(!PROBE(sample_op,&after) && equal_resources(&stable,&after));
                if (policy==2) CHECK(release_counts(0,0));
            }
            for (unsigned i=0;i<3;++i) {
                CHECK(!PROBE(PROBE_FAIL_AFTER,i));
                CHECK(call(SYS_MMAP,base+8192,2,0)==-12);
                CHECK(!PROBE(sample_op,&after) && equal_resources(&stable,&after));
                if (policy==2) CHECK(release_counts(0,0));
            }
        }
        CHECK(!PROBE(sample_op,&stable));
        for (unsigned i=0;i<4;++i) {
            CHECK(!PROBE(PROBE_FAIL_AFTER,i));
            CHECK(read(fd,cross,4)==-12);
            CHECK(!p[4094] && p[4096]==27);
            CHECK(!PROBE(sample_op,&after));
            if (mode==5) CHECK(equal_resources(&stable,&after));
            if (mode==5 && policy==2) CHECK(release_counts(0,0));
            /* Sibling exits can change global pool balances between syscalls. */
        }
        CHECK(read(fd,cross,4)==4 && p[4094]=='W' && p[4096]=='l');
        if (mode==5 && policy==2) CHECK(release_counts(2,2));
        close(fd);
        unsigned char id=call(SYS_GETPID,0,0,0);
        p[0]=id;
        call(SYS_SLEEP,1,0,0);
        CHECK(p[0]==id);
        CHECK(!call(SYS_MUNMAP,base,2,0));
        CHECK(call(SYS_MMAP,base,2,0)==base && !p[0] && !p[4096]);
        p[0]=call(SYS_GETPID,0,0,0);
        return 73;
    }
    CHECK(rc==1);
    /* Materialize the creator's stack before injecting allocation failures. */
    CHECK(!PROBE(PROBE_SNAPSHOT,&stable));
    CHECK(!PROBE(PROBE_SNAPSHOT,&stable));
    if (mode==0) {
        for (unsigned i=0;i<8;++i) {
            CHECK(!PROBE(PROBE_FAIL_AFTER,i));
            CHECK(SPAWN()==-12);
            CHECK(!PROBE(PROBE_SNAPSHOT,&after) && equal_resources(&stable,&after));
        }
    }
    unsigned n=(mode==0 || mode==4) ? 8 : 1;
    for (unsigned i=0;i<n;++i) CHECK(SPAWN()>0);
    if (mode==5 && policy==2) CHECK(!PROBE(PROBE_START,0));
    CHECK(!RELEASE() && SPAWN()==-22);
    CHECK(*p==19 && p[4096]==27);
    CHECK(!call(SYS_MUNMAP,base,pages,0));
    if (mode==4) call(SYS_SLEEP,100,0,0);
    unsigned good=0, fault=0;
    for (unsigned i=0;i<n;++i) {
        int status=0;
        CHECK(call(SYS_WAIT,(long)&status,0,0)>0);
        if (status==73) ++good;
        else if (status==-1) ++fault;
        else return status ? status : __LINE__;
    }
    if (mode==4) { CHECK(fault && good+fault==n); }
    else if (mode==1 || mode==2) { CHECK(fault==n); }
    else { CHECK(good==n); }
    CHECK(!PROBE(PROBE_SNAPSHOT,&after) && equal_resources(&original,&after));
    return 0;
}

int main(void)
{
    for (unsigned policy=0;policy<3;++policy)
        for (unsigned mode=0;mode<6;++mode) {
            int result=check_policy(policy,mode);
            if (result) return result;
        }
    say("TC PASS\n");
    return 0;
}

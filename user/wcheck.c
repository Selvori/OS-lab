/* Sparse, disjoint borrowers with explicitly sequenced lifetime changes. */
#include "lib.h"
#include <kernel/lifecycle_probe.h>

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
#define PROBE(op,arg) call(SYS_LIFECYCLE_PROBE,op,(long)(arg),0)
#define BASE 0x200000L
#define PAGE 4096
#define SPAN 0x200000
#define MAP(va,n) call(SYS_MMAP,(long)(va),(n),0)
#define UNMAP(va,n) call(SYS_MUNMAP,(long)(va),(n),0)

static bool publish(const char *path, int value)
{
    long fd=open(path,OPEN_CREATE|OPEN_WRONLY);
    if (fd<0) return false;
    long n=write(fd,&value,sizeof(value));
    close(fd);
    return n==sizeof(value);
}

static int await(const char *path)
{
    for (;;) {
        long fd=open(path,OPEN_RDONLY);
        if (fd>=0) {
            int value;
            long n=read(fd,&value,sizeof(value));
            close(fd);
            if (n==sizeof(value)) return value;
        }
        call(SYS_YIELD,0,0,0);
    }
}

static bool resources_equal(const struct lifecycle_sample *a, const struct lifecycle_sample *b)
{
    return a->user_left==b->user_left && a->kernel_left==b->kernel_left &&
           a->nodes_left==b->nodes_left && a->slots_used==b->slots_used &&
           a->files_used==b->files_used;
}

static bool released(unsigned windows, unsigned frames, unsigned branches, unsigned slots)
{
    struct template_stats s;
    return !PROBE(PROBE_TEMPLATE,&s) && s.released_windows==windows &&
           s.dropped_refs==frames && s.close_scan_branches==branches &&
           s.close_scan_slots==slots;
}

static unsigned long refs(unsigned long va)
{
    struct lifecycle_mapping m;
    m.address=va;
    return PROBE(PROBE_MAPPING,&m) ? 0 : m.refs;
}

static int child(volatile unsigned char *p, unsigned mode)
{
    bool first=await("/w-a")==call(SYS_GETPID,0,0,0);
    await("/w-g");
    CHECK(p[0]==19 && p[2*PAGE]==29 && p[SPAN]==37);
    if (first) {
        p[2*PAGE]=49;
        /* A slot absent from the image becomes private. */
        CHECK(MAP(p+PAGE,1)==BASE+PAGE && !p[PAGE]);
        p[PAGE]=59;
    } else {
        CHECK(!UNMAP(p,1) && MAP(p,1)==BASE && !p[0]);
        p[0]=69;
        CHECK(!UNMAP(p+SPAN,1));
    }
    CHECK(p[0]==(first ? 19 : 69) && p[2*PAGE]==(first ? 49 : 29));
    CHECK(publish(first ? "/w-r" : "/w-s",1));
    if (first) {
        await("/w-d");
        if (mode==1) return 73;
        const char *args[]={"/bin/wcheck","hold",0};
        return call(SYS_EXEC,(long)args[0],(long)args,0);
    }
    await("/w-k");
    CHECK(p[0]==69 && p[2*PAGE]==29);
    p[2*PAGE]=79;
    CHECK(publish("/w-y",1));
    await("/w-z");
    CHECK(p[0]==69 && p[2*PAGE]==79);
    return 73;
}

static int dual(unsigned mode)
{
    struct lifecycle_sample before, after;
    CHECK(!PROBE(PROBE_SNAPSHOT,&before));
    CHECK(MAP(BASE,1)==BASE && MAP(BASE+2*PAGE,1)==BASE+2*PAGE &&
          MAP(BASE+SPAN,1)==BASE+SPAN);
    volatile unsigned char *p=(void *)BASE;
    p[0]=19; p[2*PAGE]=29; p[SPAN]=37;
    long rc=call(SYS_FREEZE,2,0,0);
    if (!rc) return child(p,mode);
    CHECK(rc==1);
    long a=call(SYS_SPAWN,0,0,0);
    long b=call(SYS_SPAWN,0,0,0);
    CHECK(a>0 && b>0 && publish("/w-a",a));
    CHECK(!PROBE(PROBE_START,0));
    if (mode==1) CHECK(!call(SYS_RELEASE,0,0,0));
    CHECK(publish("/w-g",1));
    await("/w-r"); await("/w-s");
    if (mode!=1) CHECK(!call(SYS_RELEASE,0,0,0));
    CHECK(call(SYS_SPAWN,0,0,0)==-22 && call(SYS_RELEASE,0,0,0)==-22);
    unsigned scans=mode==1 ? 2048 : 1536;
    CHECK(released(0,0,2,scans));
    CHECK(p[0]==19 && p[2*PAGE]==29 && p[SPAN]==37);
    if (mode==2) {
        CHECK(refs(BASE)==2 && refs(BASE+2*PAGE)==2 && refs(BASE+SPAN)==2);
    } else {
        CHECK(!UNMAP(p,1) && !UNMAP(p+2*PAGE,1) && !UNMAP(p+SPAN,1));
    }
    CHECK(publish("/w-d",1));
    if (mode!=1) {
        await("/w-x");
        /* A already exec'd, and both A and B are still alive. B borrows a
         * different page of the first window, so that window stays held. */
        CHECK(released(1,1,2,scans));
        if (mode==2) CHECK(refs(BASE)==2 && refs(BASE+SPAN)==1);
        CHECK(publish("/w-h",1));
    }
    int status;
    CHECK(call(SYS_WAIT,(long)&status,0,0)==a && status==73);
    CHECK(released(1,1,2,scans));
    CHECK(publish("/w-k",1));
    await("/w-y");
    CHECK(released(2,3,2,scans));
    if (mode==2) {
        CHECK(refs(BASE)==1 && refs(BASE+2*PAGE)==1 && refs(BASE+SPAN)==1);
        CHECK(p[0]==19 && p[2*PAGE]==29 && p[SPAN]==37);
        CHECK(!UNMAP(p,1) && !UNMAP(p+2*PAGE,1) && !UNMAP(p+SPAN,1));
    }
    CHECK(publish("/w-z",1));
    CHECK(call(SYS_WAIT,(long)&status,0,0)==b && status==73);
    const char *markers[]={"/w-a","/w-g","/w-r","/w-s","/w-d","/w-k","/w-y","/w-z",
                           "/w-x","/w-h"};
    for (unsigned i=0;i<(mode==1 ? 8U : 10U);++i)
        CHECK(!call(SYS_UNLINK,(long)markers[i],0,0));
    CHECK(!PROBE(PROBE_STOP,&after) && resources_equal(&before,&after));
    return 0;
}

static int early_return(void)
{
    /* Close after the sole branch already replaced the original page. */
    struct lifecycle_sample before, after;
    CHECK(!PROBE(PROBE_SNAPSHOT,&before));
    CHECK(MAP(BASE,1)==BASE);
    volatile unsigned char *p=(void *)BASE;
    p[0]=93;
    long rc=call(SYS_FREEZE,2,0,0);
    if (!rc) {
        CHECK(p[0]==93);
        p[0]=94;
        CHECK(publish("/w-r",1));
        await("/w-d");
        CHECK(p[0]==94);
        return 73;
    }
    CHECK(rc==1 && call(SYS_SPAWN,0,0,0)>0);
    await("/w-r");
    CHECK(!UNMAP(p,1));
    CHECK(!PROBE(PROBE_START,0));
    CHECK(!call(SYS_RELEASE,0,0,0));
    CHECK(released(1,1,1,512));
    CHECK(publish("/w-d",1));
    int status;
    CHECK(call(SYS_WAIT,(long)&status,0,0)>0 && status==73);
    CHECK(!call(SYS_UNLINK,(long)"/w-r",0,0) && !call(SYS_UNLINK,(long)"/w-d",0,0));
    CHECK(!PROBE(PROBE_STOP,&after) && resources_equal(&before,&after));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc==2 && equal(argv[1],"hold")) {
        CHECK(publish("/w-x",1));
        await("/w-h");
        return 73;
    }
    for (unsigned mode=0;mode<3;++mode) {
        int rc=dual(mode);
        if (rc) return rc;
        say(mode==0 ? "WC write-then-close/disjoint/exec\n" :
            mode==1 ? "WC close-then-write/disjoint/wait\n" : "WC creator-refs/holes/remap\n");
    }
    int rc=early_return();
    if (rc) return rc;
    say("WC close-immediate\nWC PASS\n");
    return 0;
}

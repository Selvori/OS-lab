#include "lib.h"
#include <kernel/lifecycle_probe.h>
#define PAGE 4096
#define PAGES 2048
static long left(void)
{
    struct lifecycle_sample s;
    if (call(SYS_LIFECYCLE_PROBE,PROBE_SNAPSHOT,(long)&s,0)) return -1;
    return s.user_left;
}
static void number(long value)
{
    char buf[20];
    unsigned i=sizeof(buf);
    do { buf[--i]='0'+(value%10); value/=10; } while(value);
    buf[--i]=' ';
    write(1,buf+i,sizeof(buf)-i);
}
static int branch(volatile unsigned char *heap, int policy)
{
    long fd;
    while ((fd=open("/audit-go",OPEN_RDONLY)) < 0) call(SYS_YIELD,0,0,0);
    close(fd);
    const unsigned ends[]={508,1020,1532,2044,2047};
    const unsigned sizes[]={509,512,512,512,3};
    (void)left(); /* Materialize the stack before recording its page balance. */
    long before=left();
    unsigned w=0;
    for (unsigned k=0;k<PAGES;k++) {
        if (k==ends[w]) { ++w; continue; }
        heap[k*PAGE]=1;
    }
    long observed[6];
    observed[0]=before-left();
    if (observed[0]!=2043) return 51;
    unsigned released=0;
    for (w=0;w<5;w++) {
        if (heap[ends[w]*PAGE]!=0) return 52;
        heap[ends[w]*PAGE]=1;
        if (policy==2) released+=sizes[w];
        observed[w+1]=before-left();
        if (observed[w+1]!=2044+(long)w-released) return 53+w;
    }
    for (unsigned k=0;k<PAGES;k++) if (heap[k*PAGE]!=1) return 59;
    say("DELTA"); for (w=0;w<6;w++) number(observed[w]); say("\n");
    return 73;
}
int main(int argc,char **argv)
{
    if (argc!=2) return 1;
    int policy=argv[1][0]-'0';
    (void)left(); /* Materialize the stack before recording its page balance. */
    long before=left();
    long addr=call(SYS_MMAP,0,PAGES,0);
    if (addr!=0x3000) return 2;
    long frozen=call(SYS_FREEZE,policy,0,0);
    if (!frozen) return branch((void *)addr,policy);
    if (frozen!=1 || call(SYS_SPAWN,0,0,0)<0 || call(SYS_RELEASE,0,0,0)) return 3;
    if (call(SYS_MUNMAP,addr,PAGES,0)) return 4;
    long fd=open("/audit-go",OPEN_CREATE|OPEN_WRONLY);
    if (fd<0) return 5;
    close(fd);
    int status=0;
    if (call(SYS_WAIT,(long)&status,0,0)<0 || status!=73) return status ? status : 6;
    if (call(SYS_UNLINK,(long)"/audit-go",0,0) || left()!=before) return 7;
    say("INDEPENDENT WINDOW AUDIT PASS\n");
    return 0;
}

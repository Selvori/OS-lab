/* Hold the last branch after its creator dropped the original address range.
 * Filesystem marker makes the sampling order independent of scheduling. */
#include "lib.h"
#include <kernel/lifecycle_probe.h>

static void number(unsigned long x)
{
    char buf[22];
    unsigned i=sizeof(buf);
    do { buf[--i]='0'+x%10; x/=10; } while (x);
    buf[--i]=' ';
    write(1,buf+i,sizeof(buf)-i);
}

int main(int argc,char **argv)
{
    if (argc!=2) return 1;
    int policy=argv[1][0]-'0';
    const char *marker="/pin-ready";
    struct lifecycle_sample before,sample;
    call(SYS_LIFECYCLE_PROBE,PROBE_SNAPSHOT,(long)&before,0);
    long addr=call(SYS_MMAP,0,2048,0);
    if (addr<0) return 2;
    volatile unsigned char *p=(void *)addr;
    long rc=policy ? call(SYS_FREEZE,policy-1,0,0) : call(SYS_FORK,0,0,0);
    if (!rc) {
        long fd;
        while ((fd=open(marker,OPEN_RDONLY))<0) call(SYS_YIELD,0,0,0);
        close(fd);
        for (unsigned i=0;i<2048;++i) p[i*4096]=i;
        call(SYS_LIFECYCLE_PROBE,PROBE_SNAPSHOT,(long)&sample,0);
        say("PIN"); number(before.user_left-sample.user_left);
        number(before.kernel_left-sample.kernel_left); say("\n");
        call(SYS_SLEEP,5,0,0);
        for (unsigned i=0;i<2048;++i) if (p[i*4096]!=(unsigned char)i) return 3;
        return 73;
    }
    if (rc<0) return 4;
    if (policy && (call(SYS_SPAWN,0,0,0)<0 || call(SYS_RELEASE,0,0,0))) return 5;
    if (call(SYS_MUNMAP,addr,2048,0)) return 6;
    long fd=open(marker,OPEN_CREATE|OPEN_WRONLY);
    if (fd<0) return 7;
    close(fd);
    int status;
    if (call(SYS_WAIT,(long)&status,0,0)<0 || status!=73) return 8;
    if (call(SYS_UNLINK,(long)marker,0,0)) return 9;
    call(SYS_LIFECYCLE_PROBE,PROBE_SNAPSHOT,(long)&sample,0);
    if (sample.user_left!=before.user_left || sample.kernel_left!=before.kernel_left ||
        sample.slots_used!=before.slots_used || sample.nodes_left!=before.nodes_left ||
        sample.files_used!=before.files_used) return 10;
    say("PIN PASS\n");
    return 0;
}

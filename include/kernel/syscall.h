#ifndef SEELE_SYSCALL_H
#define SEELE_SYSCALL_H
#define SYS_WRITE 1
#define SYS_GETPID 2
#define SYS_TICKS 3
#define SYS_EXIT 4
#define SYS_MMAP 5
#define SYS_MUNMAP 6
#define SYS_FORK 7
#define SYS_WAIT 8
#define SYS_SLEEP 9
#define SYS_YIELD 10
#define SYS_OPEN 11
#define SYS_READ 12
#define SYS_CLOSE 13
#define SYS_DUP 14
#define SYS_FSTAT 15
#define SYS_READDIR 16
#define SYS_MKDIR 17
#define SYS_UNLINK 18
#define SYS_CHDIR 19
#define SYS_GETCWD 20
#define SYS_SYNC 21
#define SYS_EXEC 22
#define SYS_FREEZE 23
#define SYS_SPAWN 24
#define SYS_RELEASE 25
#ifndef __ASSEMBLER__
struct proc;
void syscall_dispatch(struct proc *p);
#endif
#endif

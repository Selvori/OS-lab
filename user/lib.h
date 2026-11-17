#ifndef USER_LIB_H
#define USER_LIB_H

#include <kernel/file_abi.h>
#include <kernel/fs.h>
#include <kernel/syscall.h>

static inline long call(long number, long first, long second, long third)
{
    register long a0 __asm__("a0") = first;
    register long a1 __asm__("a1") = second;
    register long a2 __asm__("a2") = third;
    register long a7 __asm__("a7") = number;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline long open(const char *path, unsigned flags)
{
    return call(SYS_OPEN, (long)path, flags, 0);
}

static inline long read(long fd, void *data, size_t count)
{
    return call(SYS_READ, fd, (long)data, count);
}

static inline long write(long fd, const void *data, size_t count)
{
    return call(SYS_WRITE, fd, (long)data, count);
}

static inline void close(long fd)
{
    call(SYS_CLOSE, fd, 0, 0);
}

size_t length(const char *text);
bool equal(const char *a, const char *b);
long write_text(long fd, const char *text);
void say(const char *text);

#endif

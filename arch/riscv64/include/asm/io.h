#ifndef OSLAB_IO_H
#define OSLAB_IO_H
#include <kernel/types.h>

/* volatile 保证实际访问；fence 同时约束编译器和 RISC-V 的内存/I/O 顺序。 */
static inline void io_fence(void)
{
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}
static inline uint8 mmio_read8(uint64 address)
{
    io_fence();
    uint8 value = *(volatile uint8 *)address;
    io_fence();
    return value;
}
static inline uint32 mmio_read32(uint64 address)
{
    io_fence();
    uint32 value = *(volatile uint32 *)address;
    io_fence();
    return value;
}
static inline void mmio_write8(uint64 address, uint8 value)
{
    io_fence();
    *(volatile uint8 *)address = value;
    io_fence();
}
static inline void mmio_write32(uint64 address, uint32 value)
{
    io_fence();
    *(volatile uint32 *)address = value;
    io_fence();
}
#endif

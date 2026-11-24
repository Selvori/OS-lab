#ifndef OSLAB_SBI_H
#define OSLAB_SBI_H
#include <kernel/types.h>
long sbi_call(uint64 extension, uint64 function, uint64 x, uint64 y, uint64 z);
long sbi_set_timer(uint64 deadline);
#ifdef DUAL_HART_USERS
long sbi_send_ipi(uint64 hart);
#endif
void sbi_putchar(char c);
#endif

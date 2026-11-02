#ifndef SEELE_USER_H
#define SEELE_USER_H
#include <asm/sv39.h>
#define USER_BASE PAGE_SIZE
#define VA_MAX SV39_MAX_LOW_VA
#define TRAMPOLINE (VA_MAX - PAGE_SIZE)
#define TRAPFRAME (TRAMPOLINE - PAGE_SIZE)
#define USER_STACK (TRAPFRAME - PAGE_SIZE)
#define USER_KSTACK (USER_STACK - PAGE_SIZE)
struct proc;
int user_vm_create(struct proc *p);
int user_copyin(pagetable_t root, void *dst, uintptr_t src, size_t count);
#endif

#ifndef OSLAB_ASM_TRAP_H
#define OSLAB_ASM_TRAP_H

/* 汇编和 C 共用同一布局，所有偏移以字节计。x0 保留一个槽位。 */
#define TF_X(n)       ((n) * 8)
#define TF_SSTATUS    256
#define TF_SEPC       264
#define TF_SCAUSE     272
#define TF_STVAL      280
#define TF_SIZE       288

#ifndef __ASSEMBLER__
#include <kernel/types.h>

/* 当前只支持 S-mode 内核整数上下文；不支持用户栈和浮点/向量状态。 */
struct kernel_trapframe {
    uint64 x[32];
    uint64 sstatus;
    uint64 sepc;
    uint64 scause;
    uint64 stval;
};

_Static_assert(offsetof(struct kernel_trapframe, x[1]) == TF_X(1), "x offset");
_Static_assert(offsetof(struct kernel_trapframe, x[31]) == TF_X(31), "x end");
_Static_assert(offsetof(struct kernel_trapframe, sstatus) == TF_SSTATUS, "sstatus offset");
_Static_assert(offsetof(struct kernel_trapframe, sepc) == TF_SEPC, "sepc offset");
_Static_assert(offsetof(struct kernel_trapframe, scause) == TF_SCAUSE, "scause offset");
_Static_assert(offsetof(struct kernel_trapframe, stval) == TF_STVAL, "stval offset");
_Static_assert(sizeof(struct kernel_trapframe) == TF_SIZE, "trapframe size");
_Static_assert(TF_SIZE % 16 == 0, "C ABI stack alignment");
#endif
#endif

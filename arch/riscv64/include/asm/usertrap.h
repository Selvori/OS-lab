#ifndef SEELE_USERTRAP_H
#define SEELE_USERTRAP_H
#define UTF_X(n) ((n) * 8)
#define UTF_EPC 256
#define UTF_KSATP 264
#define UTF_KSP 272
#define UTF_HANDLER 280
#define UTF_HART 288
#define UTF_GP 296
#ifndef __ASSEMBLER__
#include <kernel/types.h>
struct user_trapframe {
    uint64 x[32];
    uint64 epc, kernel_satp, kernel_sp, kernel_trap, kernel_hart, kernel_gp;
};
_Static_assert(offsetof(struct user_trapframe, x[31]) == UTF_X(31), "user x31");
_Static_assert(offsetof(struct user_trapframe, epc) == UTF_EPC, "user epc");
_Static_assert(offsetof(struct user_trapframe, kernel_satp) == UTF_KSATP, "kernel satp");
_Static_assert(offsetof(struct user_trapframe, kernel_sp) == UTF_KSP, "kernel sp");
_Static_assert(offsetof(struct user_trapframe, kernel_trap) == UTF_HANDLER, "handler");
_Static_assert(offsetof(struct user_trapframe, kernel_hart) == UTF_HART, "hart");
_Static_assert(offsetof(struct user_trapframe, kernel_gp) == UTF_GP, "gp");
_Static_assert(sizeof(struct user_trapframe) == 304, "user frame size");
void trap_user_handler(void) __attribute__((noreturn));
void trap_user_return(void) __attribute__((noreturn));
#endif
#endif

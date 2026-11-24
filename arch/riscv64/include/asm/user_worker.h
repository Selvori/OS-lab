#ifndef SEELE_USER_WORKER_H
#define SEELE_USER_WORKER_H
#ifdef DUAL_HART_USERS
#include <asm/usertrap.h>

/* Owner publishes one user context and collects one completed trap. */
void user_worker_submit(struct user_trapframe *tf, pagetable_t root);
bool user_worker_collect(uint64 *cause, uint64 *value);
void user_worker_loop(void) __attribute__((noreturn));
void trap_user_resume_remote(void) __attribute__((noreturn));
#endif
#endif

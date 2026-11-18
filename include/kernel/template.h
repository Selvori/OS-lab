#ifndef SEELE_TEMPLATE_H
#define SEELE_TEMPLATE_H
#include <kernel/user.h>
struct proc;
#define HEAP_WINDOWS 9
struct split_plan { pagetable_t fresh[HEAP_WINDOWS]; };
int branch_attach(struct proc *p, struct proc *image);
void branch_detach(struct proc *p);
int branch_prepare(struct proc *p, uintptr_t begin, uintptr_t end, struct split_plan *plan);
void branch_commit(struct proc *p, struct split_plan *plan);
void branch_cancel(struct split_plan *plan);
void branch_undo(struct proc *p, struct split_plan *plan);
bool branch_borrows(struct proc *p, uintptr_t va);
void branch_own(struct proc *p, uintptr_t va);
void branch_unmap(struct proc *p, uintptr_t begin, size_t pages);
long template_freeze(unsigned policy);
long template_spawn(void);
long template_close(struct proc *p);
void template_put(struct proc *image);
#endif

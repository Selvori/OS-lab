#ifndef SEELE_TEMPLATE_H
#define SEELE_TEMPLATE_H
#include <kernel/user.h>
struct proc;
#define HEAP_WINDOWS 9
enum template_policy {
    TEMPLATE_TABLE_REFS,
    TEMPLATE_FRAME_REFS,
    TEMPLATE_WINDOW_RECLAIM,
    TEMPLATE_TABLE_REFS_BATCH,
    TEMPLATE_WINDOW_BATCH,
    TEMPLATE_WINDOW_INCREMENTAL,
    TEMPLATE_PAGE_RECLAIM,
#ifdef LIFECYCLE_CHECKS
    TEMPLATE_RECORD_ONLY,
#endif
    TEMPLATE_POLICY_COUNT,
};
struct split_plan { pagetable_t fresh[HEAP_WINDOWS]; };
int branch_attach(struct proc *p, struct proc *image);
void branch_detach(struct proc *p);
int branch_prepare(struct proc *p, uintptr_t begin, uintptr_t end, struct split_plan *plan);
void branch_commit(struct proc *p, struct split_plan *plan);
void branch_cancel(struct split_plan *plan);
void branch_undo(struct proc *p, struct split_plan *plan);
bool branch_borrows(struct proc *p, uintptr_t va);
void branch_map(struct proc *p, uintptr_t va);
void branch_drop_borrow(struct proc *p, uintptr_t va);
bool branch_count_borrows(struct proc *p, struct proc *image);
void template_reclaim_unborrowed(struct proc *image);
void branch_unmap(struct proc *p, uintptr_t begin, size_t pages);
long template_freeze(unsigned policy);
long template_spawn(void);
long template_close(struct proc *p);
int template_index_init(struct proc *image, unsigned policy);
void template_index_span(struct proc *image, uintptr_t begin, uintptr_t end);
bool template_index_drained(const struct proc *image);
#ifdef LIFECYCLE_CHECKS
long template_audit(unsigned pid);
#endif
void template_index_destroy(struct proc *image);
void template_close_job(struct proc *image);
bool template_reclaim_step(bool idle);
#ifdef LIFECYCLE_PROBE
unsigned template_work_pending(void);
#endif
const struct proc *proc_slot(unsigned slot);
void template_put(struct proc *image);
#endif

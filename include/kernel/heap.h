#ifndef SEELE_HEAP_H
#define SEELE_HEAP_H
#include <kernel/user.h>
#define USER_HEAP_START (USER_BASE + 2 * PAGE_SIZE)
#define USER_HEAP_PAGES 4096UL
#define USER_HEAP_END (USER_HEAP_START + USER_HEAP_PAGES * PAGE_SIZE)
#define N_MMAP 128
_Static_assert(USER_HEAP_END <= USER_KSTACK - PAGE_SIZE, "heap overlaps guard");
typedef struct mmap_region {
    uintptr_t begin;
    size_t npages;
    struct mmap_region *next;
} mmap_region_t;
typedef struct mmap_region_node {
    mmap_region_t mmap;
    struct mmap_region_node *next;
} mmap_region_node_t;
void mmap_pool_init(void);
size_t mmap_pool_free_count(void);
int user_heap_init(struct proc *p);
long user_heap_alloc(struct proc *p, uintptr_t hint, size_t npages);
int user_heap_free(struct proc *p, uintptr_t begin, size_t npages);
void user_heap_destroy(struct proc *p);
#endif

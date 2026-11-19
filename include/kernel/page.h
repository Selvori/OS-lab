#ifndef OSLAB_PAGE_H
#define OSLAB_PAGE_H

#include <kernel/types.h>

#define PAGE_SHIFT 12UL
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1UL)
#define PAGE_USER_REF_BATCH_MAX 32
#define PAGE_ROUND_DOWN(value) ((uintptr_t)(value) & ~PAGE_MASK)
#define PAGE_ROUND_UP(value) (((uintptr_t)(value) + PAGE_MASK) & ~PAGE_MASK)

typedef enum {
    PAGE_ZONE_KERNEL = 0,
    PAGE_ZONE_USER = 1,
    PAGE_ZONE_COUNT = 2,
} page_zone_type_t;

void page_allocator_init(void);
void *page_alloc(page_zone_type_t zone);
/* User pages carry one reference after allocation. page_free drops ownership. */
void page_retain_user(void *page);
/* One owned reference per entry. Release uses the array as scratch space;
 * callers must revoke any mappings before handing over their references. */
void page_retain_user_batch(void *const *pages, size_t count);
void page_release_user_batch(void **pages, size_t count);
unsigned page_user_refs(void *page);
void page_free(void *page);
uint64 page_free_count(page_zone_type_t zone);
uintptr_t page_zone_start(page_zone_type_t zone);
uintptr_t page_zone_end(page_zone_type_t zone);

#endif

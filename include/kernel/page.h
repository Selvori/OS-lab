#ifndef OSLAB_PAGE_H
#define OSLAB_PAGE_H

#include <kernel/types.h>

#define PAGE_SHIFT 12UL
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1UL)
#define PAGE_ROUND_DOWN(value) ((uintptr_t)(value) & ~PAGE_MASK)
#define PAGE_ROUND_UP(value) (((uintptr_t)(value) + PAGE_MASK) & ~PAGE_MASK)

typedef enum {
    PAGE_ZONE_KERNEL = 0,
    PAGE_ZONE_USER = 1,
    PAGE_ZONE_COUNT = 2,
} page_zone_type_t;

void page_allocator_init(void);
void *page_alloc(page_zone_type_t zone);
void page_free(void *page);
uint64 page_free_count(page_zone_type_t zone);
uintptr_t page_zone_start(page_zone_type_t zone);
uintptr_t page_zone_end(page_zone_type_t zone);

#endif

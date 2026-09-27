#include <kernel/lock.h>
#include <kernel/page.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <platform.h>

struct free_page {
    struct free_page *next;
};

struct page_zone {
    spinlock_t lock;
    struct free_page *freelist;
    uint64 free_count;
    uintptr_t start;
    uintptr_t end;
};

static struct page_zone zones[PAGE_ZONE_COUNT];

extern char kernel_end[];

static void zone_init(page_zone_type_t type, const char *name,
                      uintptr_t start, uintptr_t end)
{
    struct page_zone *zone = &zones[type];

    if ((start & PAGE_MASK) != 0 || (end & PAGE_MASK) != 0 || start > end)
        panic("page zone alignment");

    spinlock_init(&zone->lock, name);
    zone->freelist = NULL;
    zone->free_count = 0;
    zone->start = start;
    zone->end = end;

    for (uintptr_t address = end; address > start; address -= PAGE_SIZE) {
        struct free_page *page = (struct free_page *)(address - PAGE_SIZE);
        page->next = zone->freelist;
        zone->freelist = page;
        zone->free_count++;
    }
}

void page_allocator_init(void)
{
    uintptr_t free_start = PAGE_ROUND_UP(kernel_end);
    uintptr_t free_end = PAGE_ROUND_DOWN(DRAM_END);

    if (free_start < DRAM_BASE || free_start >= free_end)
        panic("no allocatable memory");

    uint64 total_pages = (free_end - free_start) / PAGE_SIZE;
    uint64 kernel_pages = total_pages / 4;

    if (kernel_pages == 0 || kernel_pages == total_pages)
        panic("invalid page zone split");

    uintptr_t split = free_start + kernel_pages * PAGE_SIZE;
    zone_init(PAGE_ZONE_KERNEL, "kernel-pages", free_start, split);
    zone_init(PAGE_ZONE_USER, "user-pages", split, free_end);
}

void *page_alloc(page_zone_type_t type)
{
    if (type >= PAGE_ZONE_COUNT)
        panic("invalid page zone");

    struct page_zone *zone = &zones[type];
    struct free_page *page;

    spinlock_acquire(&zone->lock);
    page = zone->freelist;
    if (page != NULL) {
        zone->freelist = page->next;
        zone->free_count--;
    }
    spinlock_release(&zone->lock);

    if (page != NULL)
        memset(page, 0, PAGE_SIZE);

    return page;
}

static struct page_zone *zone_for_address(uintptr_t address)
{
    for (unsigned i = 0; i < PAGE_ZONE_COUNT; ++i) {
        if (address >= zones[i].start && address < zones[i].end)
            return &zones[i];
    }
    return NULL;
}

void page_free(void *memory)
{
    uintptr_t address = (uintptr_t)memory;

    if (memory == NULL || (address & PAGE_MASK) != 0)
        panic("invalid page free");

    struct page_zone *zone = zone_for_address(address);
    if (zone == NULL)
        panic("page outside zones");

    memset(memory, 0xa5, PAGE_SIZE);
    struct free_page *page = memory;

    spinlock_acquire(&zone->lock);
    page->next = zone->freelist;
    zone->freelist = page;
    zone->free_count++;
    spinlock_release(&zone->lock);
}

uint64 page_free_count(page_zone_type_t type)
{
    if (type >= PAGE_ZONE_COUNT)
        panic("invalid page zone");

    struct page_zone *zone = &zones[type];
    uint64 count;

    spinlock_acquire(&zone->lock);
    count = zone->free_count;
    spinlock_release(&zone->lock);
    return count;
}

uintptr_t page_zone_start(page_zone_type_t type)
{
    if (type >= PAGE_ZONE_COUNT)
        panic("invalid page zone");
    return zones[type].start;
}

uintptr_t page_zone_end(page_zone_type_t type)
{
    if (type >= PAGE_ZONE_COUNT)
        panic("invalid page zone");
    return zones[type].end;
}

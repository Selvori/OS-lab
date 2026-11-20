#include <kernel/lifecycle_probe.h>
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
/* Indexed from the user zone's start and protected by its allocator lock. */
static uint16_t user_refs[DRAM_SIZE / PAGE_SIZE];

static uint16_t *user_ref(uintptr_t address)
{
    struct page_zone *zone = &zones[PAGE_ZONE_USER];
    assert(address >= zone->start && address < zone->end &&
           (address & PAGE_MASK) == 0, "invalid user frame");
    return &user_refs[(address - zone->start) / PAGE_SIZE];
}

void page_retain_user(void *page)
{
    uint16_t *refs = user_ref((uintptr_t)page);
    spinlock_t *lock = &zones[PAGE_ZONE_USER].lock;
    spinlock_acquire(lock);
    assert(*refs && *refs < UINT16_MAX, "retain invalid user frame");
    ++*refs;
    spinlock_release(lock);
}

void page_retain_user_batch(void *const *pages, size_t count)
{
    assert(count && count <= PAGE_USER_REF_BATCH_MAX, "user retain batch size");
    struct page_zone *zone = &zones[PAGE_ZONE_USER];

    spinlock_acquire(&zone->lock);
    for (size_t i = 0; i < count; ++i) {
        uint16_t *refs = user_ref((uintptr_t)pages[i]);
        assert(*refs && *refs < UINT16_MAX, "retain invalid user frame");
        ++*refs;
    }
    spinlock_release(&zone->lock);
}

unsigned page_user_refs(void *page)
{
    uint16_t *refs = user_ref((uintptr_t)page);
    spinlock_t *lock = &zones[PAGE_ZONE_USER].lock;
    spinlock_acquire(lock);
    unsigned count = *refs;
    spinlock_release(lock);
    return count;
}

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
#ifdef LIFECYCLE_CHECKS
    if (lifecycle_alloc_fails())
        return NULL;
#endif
    if (type >= PAGE_ZONE_COUNT)
        panic("invalid page zone");

    struct page_zone *zone = &zones[type];
    struct free_page *page;

    spinlock_acquire(&zone->lock);
    page = zone->freelist;
    if (page != NULL) {
        zone->freelist = page->next;
        zone->free_count--;
        if (type == PAGE_ZONE_USER) {
            uint16_t *refs = user_ref((uintptr_t)page);
            assert(*refs == 0, "allocated live user frame");
            *refs = 1;
        }
    }
    spinlock_release(&zone->lock);

    if (page != NULL) {
        memset(page, 0, PAGE_SIZE);
        lifecycle_page(type == PAGE_ZONE_USER, true);
    }

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

bool page_free(void *memory)
{
    uintptr_t address = (uintptr_t)memory;

    if (memory == NULL || (address & PAGE_MASK) != 0)
        panic("invalid page free");

    struct page_zone *zone = zone_for_address(address);
    if (zone == NULL)
        panic("page outside zones");

    if (zone == &zones[PAGE_ZONE_USER]) {
        uint16_t *refs = user_ref(address);
        spinlock_acquire(&zone->lock);
        assert(*refs != 0, "release idle user frame");
        bool last = --*refs == 0;
        spinlock_release(&zone->lock);
        if (!last)
            return false;
    }
    /* The last releaser owns the frame until it publishes it on the free list. */
    memset(memory, 0xa5, PAGE_SIZE);
    struct free_page *page = memory;

    spinlock_acquire(&zone->lock);
    page->next = zone->freelist;
    zone->freelist = page;
    zone->free_count++;
    spinlock_release(&zone->lock);
#ifdef LIFECYCLE_PROBE
    if (zone == &zones[PAGE_ZONE_USER])
        lifecycle_original_watch(address, ORIGINAL_RETURN);
#endif
    lifecycle_page(zone == &zones[PAGE_ZONE_USER], false);
    return true;
}

size_t page_release_user_batch(void **pages, size_t count)
{
    assert(count && count <= PAGE_USER_REF_BATCH_MAX, "user release batch size");
    struct page_zone *zone = &zones[PAGE_ZONE_USER];
    size_t last = 0;

    spinlock_acquire(&zone->lock);
    for (size_t i = 0; i < count; ++i) {
        uint16_t *refs = user_ref((uintptr_t)pages[i]);
        assert(*refs != 0, "release idle user frame");
        if (--*refs == 0)
            pages[last++] = pages[i];
    }
    spinlock_release(&zone->lock);
    if (last == 0)
        return 0;

    /* Zero-reference frames remain private until the filled list is published. */
    struct free_page *head = NULL;
    struct free_page *tail = pages[0];
    for (size_t i = 0; i < last; ++i) {
        struct free_page *page = pages[i];
        memset(page, 0xa5, PAGE_SIZE);
        page->next = head;
        head = page;
    }

    spinlock_acquire(&zone->lock);
    tail->next = zone->freelist;
    zone->freelist = head;
    zone->free_count += last;
    spinlock_release(&zone->lock);
    for (size_t i = 0; i < last; ++i) {
        lifecycle_original_watch((uintptr_t)pages[i], ORIGINAL_RETURN);
        lifecycle_page(true, false);
    }
    return last;
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

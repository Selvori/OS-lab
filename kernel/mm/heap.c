#include <asm/csr.h>
#include <kernel/heap.h>
#include <kernel/proc.h>
#include <kernel/arch.h>
#include <kernel/lock.h>
#include <kernel/print.h>

static mmap_region_node_t list_mmap_region_node[N_MMAP];
static mmap_region_node_t *available;
static spinlock_t pool_lock;
static size_t available_count;

void mmap_pool_init(void)
{
    spinlock_init(&pool_lock, "mmap-nodes");
    available = NULL;
    available_count = N_MMAP;
    for (unsigned i = 0; i < N_MMAP; ++i) {
        list_mmap_region_node[i].next = available;
        available = &list_mmap_region_node[i];
    }
}

static mmap_region_t *region_alloc(void)
{
    spinlock_acquire(&pool_lock);
    mmap_region_node_t *node = available;
    if (node) {
        available = node->next;
        --available_count;
    }
    spinlock_release(&pool_lock);
    if (!node) return NULL;
    node->mmap = (mmap_region_t){0};
    return &node->mmap;
}

static void region_free(mmap_region_t *region)
{
    /* mmap is the first field; the node belongs to the static global pool. */
    mmap_region_node_t *node = (mmap_region_node_t *)region;
    spinlock_acquire(&pool_lock);
    node->next = available;
    available = node;
    ++available_count;
    spinlock_release(&pool_lock);
}

size_t mmap_pool_free_count(void)
{
    spinlock_acquire(&pool_lock);
    size_t count = available_count;
    spinlock_release(&pool_lock);
    return count;
}

static void heap_context(struct proc *p)
{
    assert(p && p->owner == arch_cpu_id() && !arch_irq_enabled(), "heap owner context");
}

static bool valid_range(struct proc *p, uintptr_t begin, size_t npages)
{
    return npages && !(begin & PAGE_MASK) && begin >= USER_HEAP_START &&
           begin < p->heap_top && npages <= (p->heap_top - begin) / PAGE_SIZE;
}

static uintptr_t region_end(mmap_region_t *region)
{
    return region->begin + region->npages * PAGE_SIZE;
}

int user_heap_init(struct proc *p)
{
    heap_context(p);
    mmap_region_t *region = region_alloc();
    if (!region) return -12;
    *region = (mmap_region_t){USER_HEAP_START, USER_HEAP_PAGES, NULL};
    p->heap_top = USER_HEAP_END;
    p->mmap = region;
    return 0;
}

long user_heap_alloc(struct proc *p, uintptr_t hint, size_t npages)
{
    heap_context(p);
    if (!npages || npages > USER_HEAP_PAGES ||
        (hint && !valid_range(p, hint, npages))) return -22;
    mmap_region_t **link = &p->mmap;
    mmap_region_t *region;
    uintptr_t begin = 0, end = 0;
    for (; (region = *link) != NULL; link = &region->next) {
        begin = hint ? hint : region->begin;
        if (begin < region->begin || begin >= region_end(region)) continue;
        if (npages > (region_end(region) - begin) / PAGE_SIZE) continue;
        end = begin + npages * PAGE_SIZE;
        break;
    }
    if (!region) return -12;
    uintptr_t old_end = region_end(region);
    bool left = begin != region->begin, right = end != old_end;
    mmap_region_t *extra = left && right ? region_alloc() : NULL;
    if (left && right && !extra) return -12;
    /* Do not publish the interval edit until every physical page is mapped. */
    size_t mapped = 0;
    for (; mapped < npages; ++mapped) {
        uintptr_t va = begin + mapped * PAGE_SIZE;
        void *page = page_alloc(PAGE_ZONE_USER);
        if (!page) break;
        if (sv39_map(p->pgtbl, va, (uintptr_t)page, PAGE_SIZE,
                     PTE_R | PTE_W | PTE_U) != 0) {
            page_free(page);
            /* walk may have allocated intermediate nodes before failing. */
            sv39_prune_empty(p->pgtbl, va);
            break;
        }
    }
    if (mapped != npages) {
        if (mapped) assert(vm_unmappages(p->pgtbl, begin, mapped) == 0, "heap rollback");
        if (extra) region_free(extra);
        return -12;
    }
    if (left) {
        if (right) {
            *extra = (mmap_region_t){end, (old_end - end) / PAGE_SIZE, region->next};
            region->next = extra;
        }
        region->npages = (begin - region->begin) / PAGE_SIZE;
    } else if (right) {
        region->begin = end;
        region->npages = (old_end - end) / PAGE_SIZE;
    } else {
        *link = region->next;
        region_free(region);
    }
    sfence_vma();
    return (long)begin;
}

int user_heap_free(struct proc *p, uintptr_t begin, size_t npages)
{
    heap_context(p);
    if (!valid_range(p, begin, npages)) return -22;
    uintptr_t end = begin + npages * PAGE_SIZE;
    mmap_region_t *prev = NULL, *next = p->mmap;
    while (next && next->begin < begin) { prev = next; next = next->next; }
    if ((prev && region_end(prev) > begin) || (next && next->begin < end)) return -22;
    /* Reject any hole, supervisor page or foreign frame before reserving metadata. */
    for (uintptr_t va = begin; va < end; va += PAGE_SIZE) {
        uintptr_t pa; uint64 flags;
        if (!sv39_lookup(p->pgtbl, va, &pa, &flags) ||
            (flags & (PTE_U | PTE_R | PTE_W | PTE_X)) != (PTE_U | PTE_R | PTE_W) ||
            pa < page_zone_start(PAGE_ZONE_USER) || pa >= page_zone_end(PAGE_ZONE_USER))
            return -22;
    }
    bool left = prev && region_end(prev) == begin;
    bool right = next && next->begin == end;
    mmap_region_t *extra = !left && !right ? region_alloc() : NULL;
    if (!left && !right && !extra) return -12;
    assert(vm_unmappages(p->pgtbl, begin, npages) == 0, "heap unmap invariant");
    if (left) {
        prev->npages += npages;
        if (right) {
            prev->npages += next->npages;
            prev->next = next->next;
            region_free(next);
        }
    } else if (right) {
        next->begin = begin;
        next->npages += npages;
    } else {
        *extra = (mmap_region_t){begin, npages, next};
        if (prev) prev->next = extra;
        else p->mmap = extra;
    }
    return 0;
}

void user_heap_destroy(struct proc *p)
{
    heap_context(p);
    if (p->pgtbl) {
        for (uintptr_t va = USER_HEAP_START; va < p->heap_top; va += PAGE_SIZE) {
            if (sv39_lookup(p->pgtbl, va, NULL, NULL))
                assert(vm_unmappages(p->pgtbl, va, 1) == 0, "heap destroy");
        }
    }
    while (p->mmap) {
        mmap_region_t *next = p->mmap->next;
        region_free(p->mmap);
        p->mmap = next;
    }
    p->heap_top = 0;
}

#include <kernel/proc.h>
#include <kernel/template.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <kernel/lifecycle_probe.h>
#include <asm/csr.h>

_Static_assert(NPROC <= 255 && HEAP_WINDOWS <= 16, "window accounting fields");

_Static_assert((USER_HEAP_END - 1) / SV39_LEAF_SPAN + 1 == HEAP_WINDOWS,
               "template directory must cover the heap");
struct branch_heap {
    struct proc *image;
    unsigned shared;
    bool mixed_heap;
    uint64 owned[HEAP_WINDOWS][8];
    uint16_t borrowed[HEAP_WINDOWS];
};
_Static_assert(sizeof(struct branch_heap) <= PAGE_SIZE, "branch metadata");

/* USER_BASE guarantees both roots already have a private low middle table. */
static pte_t *directory(struct proc *p)
{
    return (pte_t *)sv39_pte_to_pa(p->pgtbl[0]);
}

static pte_t *table(struct proc *p, unsigned window)
{
    pte_t entry = directory(p)[window];
    return entry ? (pte_t *)sv39_pte_to_pa(entry) : NULL;
}

static unsigned first_slot(unsigned window)
{
    return window ? 0 : sv39_index(0, USER_HEAP_START);
}

static void take_refs(struct proc *p, unsigned w)
{
    if (p->branch->image->template_owned) return;
    pte_t *leaf = table(p, w);
    for (unsigned i = first_slot(w); i < 512; ++i) {
        if (leaf[i] & PTE_V) {
            page_retain_user((void *)sv39_pte_to_pa(leaf[i]));
            template_count(retains, 1);
        }
    }
}

int branch_attach(struct proc *p, struct proc *image)
{
    struct branch_heap *b = page_alloc(PAGE_ZONE_KERNEL);
    if (!b) return -12;
    p->branch = b;
    b->image = image;
    ++image->template_refs;
    unsigned first = first_slot(0);
    b->mixed_heap = image->mixed_heap;
    if (b->mixed_heap) {
        memcpy(table(p, 0) + first, table(image, 0) + first,
               (512 - first) * sizeof(pte_t));
        template_count(copied_slots, 512 - first);
        take_refs(p, 0);
    }
    for (unsigned w = 1; w < HEAP_WINDOWS; ++w) {
        if (directory(image)[w]) {
            directory(p)[w] = directory(image)[w];
            b->shared |= 1U << w;
        }
    }
    return 0;
}

void branch_cancel(struct split_plan *plan)
{
    for (unsigned w = 0; w < HEAP_WINDOWS; ++w)
        if (plan->fresh[w]) page_free(plan->fresh[w]);
}

int branch_prepare(struct proc *p, uintptr_t begin, uintptr_t end, struct split_plan *plan)
{
    if (!p->branch || begin >= USER_HEAP_END || end <= USER_HEAP_START) return 0;
    if (end > USER_HEAP_END) end = USER_HEAP_END;
    for (unsigned w = begin / SV39_LEAF_SPAN; w <= (end-1) / SV39_LEAF_SPAN; ++w) {
        if (!(p->branch->shared & (1U << w))) continue;
        plan->fresh[w] = page_alloc(PAGE_ZONE_KERNEL);
        if (!plan->fresh[w]) {
            branch_cancel(plan);
            return -12;
        }
        memcpy(plan->fresh[w], table(p,w), PAGE_SIZE);
    }
    return 0;
}

void branch_commit(struct proc *p, struct split_plan *plan)
{
    for (unsigned w = 1; w < HEAP_WINDOWS; ++w) {
        if (!plan->fresh[w]) continue;
        directory(p)[w] = sv39_pa_to_pte((uintptr_t)plan->fresh[w]) | PTE_V;
        p->branch->shared &= ~(1U << w);
        take_refs(p, w);
        template_count(copied_slots, 512);
    }
}

void branch_undo(struct proc *p, struct split_plan *plan)
{
    for (unsigned w = 1; w < HEAP_WINDOWS; ++w) {
        pte_t *leaf = plan->fresh[w];
        if (!leaf) continue;
        directory(p)[w] = directory(p->branch->image)[w];
        p->branch->shared |= 1U << w;
        sfence_vma();
        if (!p->branch->image->template_owned)
            for (unsigned i = 0; i < 512; ++i)
                if (leaf[i]) page_free((void *)sv39_pte_to_pa(leaf[i]));
        page_free(leaf);
    }
}

bool branch_borrows(struct proc *p, uintptr_t va)
{
    if (!p->branch || va < USER_HEAP_START || va >= USER_HEAP_END) return false;
    unsigned w = va / SV39_LEAF_SPAN, i = sv39_index(0, va);
    if (p->branch->shared & (1U << w)) return true;
    return p->branch->image->template_owned &&
           !(p->branch->owned[w][i/64] & (1UL << (i%64)));
}

/* Called at close, while the owner serializes mapping changes and branch teardown.
 * An original heap frame is still borrowed only if its branch slot is valid and
 * has not been replaced by a private mapping. Holes need no history. */
bool branch_count_borrows(struct proc *p, struct proc *image)
{
    struct branch_heap *b = p->branch;
    if (!b || b->image != image) return false;
    template_count(close_scan_branches, 1);
    for (unsigned w = 0; w < HEAP_WINDOWS; ++w) {
        if (!w && !b->mixed_heap) continue;
        pte_t *leaf = table(p, w);
        if (!leaf) continue;
        unsigned count = 0, first = first_slot(w);
        for (unsigned i = first; i < 512; ++i)
            if ((leaf[i] & PTE_V) && !(b->owned[w][i/64] & (1UL << (i%64))))
                ++count;
        template_count(close_scan_slots, 512 - first);
        b->borrowed[w] = count;
        if (count)
            ++image->template_borrowers[w];
    }
    return true;
}

/* The image root is never active. A branch removes its old translation and
 * fences before reporting its last borrow. Keep the directory links and page
 * table storage until the existing template reference count reaches zero. */
static void reclaim_window(struct proc *image, unsigned w)
{
    assert(image->template_tracking && !image->template_borrowers[w] &&
           !(image->template_reclaimed & (1U << w)), "template window reclaim");
    pte_t *leaf = table(image, w);
    assert(leaf, "template reclaim leaf");
    uint64 start = lifecycle_begin();
    for (unsigned i = first_slot(w); i < 512; ++i) {
        pte_t entry = leaf[i];
        if (entry & PTE_V) {
            leaf[i] = 0;
            page_free((void *)sv39_pte_to_pa(entry));
            template_count(dropped_refs, 1);
        }
    }
    image->template_reclaimed |= 1U << w;
    template_count(released_windows, 1);
#ifdef LIFECYCLE_PROBE
    if (lifecycle_active) {
        uint64 ticks = lifecycle_time() - start;
        template_stats.release_ticks += ticks;
        if (ticks > template_stats.max_release_ticks)
            template_stats.max_release_ticks = ticks;
    }
#else
    (void)start;
#endif
}

void template_reclaim_unborrowed(struct proc *image)
{
    for (unsigned w = 0; w < HEAP_WINDOWS; ++w)
        if (!image->template_borrowers[w] && (w || image->mixed_heap) && table(image, w))
            reclaim_window(image, w);
}

static void end_window_borrow(struct branch_heap *b, unsigned w)
{
    struct proc *image = b->image;
    b->borrowed[w] = 0;
    assert(image->template_borrowers[w], "template window borrow underflow");
    if (!--image->template_borrowers[w])
        reclaim_window(image, w);
}

void branch_drop_borrow(struct proc *p, uintptr_t va)
{
    struct branch_heap *b = p->branch;
    if (!b->image->template_tracking) return;
    unsigned w = va / SV39_LEAF_SPAN;
    assert(b->borrowed[w], "branch window borrow underflow");
    if (b->borrowed[w] == 1)
        end_window_borrow(b, w);
    else
        --b->borrowed[w];
}

void branch_map(struct proc *p, uintptr_t va)
{
    if (p->branch && va >= USER_HEAP_START && va < SV39_LEAF_SPAN)
        p->branch->mixed_heap = true;
    if (p->branch && p->branch->image->template_owned &&
        va >= USER_HEAP_START && va < USER_HEAP_END) {
        unsigned i = sv39_index(0, va);
        p->branch->owned[va/SV39_LEAF_SPAN][i/64] |= 1UL << (i%64);
    }
}

void branch_unmap(struct proc *p, uintptr_t va, size_t pages)
{
    for (; pages; --pages, va += PAGE_SIZE) {
        unsigned w = va / SV39_LEAF_SPAN, i = sv39_index(0, va);
        pte_t *slot = &table(p,w)[i];
        void *frame = (void *)sv39_pte_to_pa(*slot);
        bool borrowed = branch_borrows(p, va);
        *slot = 0;
        sfence_vma();
        if (borrowed)
            branch_drop_borrow(p, va);
        else
            page_free(frame);
        p->branch->owned[w][i/64] &= ~(1UL << (i%64));
        sv39_prune_empty(p->pgtbl, va);
    }
}

void branch_detach(struct proc *p)
{
    struct branch_heap *b = p->branch;
    for (unsigned w = 0; w < HEAP_WINDOWS; ++w) {
        if (!w && !b->mixed_heap) continue;
        pte_t *leaf = table(p, w);
        if (!leaf) continue;
        pte_t link = directory(p)[w];
        directory(p)[w] = 0;
        /* In the mixed window revoke heap leaves before dropping any frame.
         * Both exec and wait run on the kernel root with user translation inactive. */
        sfence_vma();
        if (b->borrowed[w])
            end_window_borrow(b, w);
        if (b->shared & (1U << w)) continue;
        if (b->image->template_owned) {
            for (unsigned j = 0; j < 8; ++j) {
                template_count(bitmap_words, 1);
                uint64 bits = b->owned[w][j];
                for (unsigned i = 64*j; bits; bits >>= 1, ++i) {
                    if (!(bits & 1)) continue;
                    page_free((void *)sv39_pte_to_pa(leaf[i]));
                    template_count(private_drops, 1);
                }
            }
        } else {
            for (unsigned i = first_slot(w); i < 512; ++i) {
                template_count(exit_slots, 1);
                if (leaf[i]) page_free((void *)sv39_pte_to_pa(leaf[i]));
            }
        }
        if (w) page_free(leaf);
        else {
            memset(leaf + first_slot(0), 0, (512-first_slot(0))*sizeof(*leaf));
            directory(p)[0] = link;
        }
    }
    sfence_vma();
    p->branch = NULL;
    template_put(b->image);
    page_free(b);
}

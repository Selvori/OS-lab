#include <kernel/lifecycle_probe.h>
#include <asm/csr.h>
#include <kernel/proc.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <kernel/heap.h>
#include <kernel/template.h>

extern char trampoline[], user_image_start[], user_image_end[];

int user_vm_create(struct proc *p, bool shared)
{
    if (!shared) {
        size_t image_size = (size_t)(user_image_end - user_image_start);
        if (image_size == 0 || image_size > PAGE_SIZE)
            return -1;
        memcpy(p->code, user_image_start, image_size);
    }
    uint64 writable = PTE_R | PTE_U | (shared ? PTE_COW : PTE_W);
    if (sv39_map(p->pgtbl, USER_BASE, (uintptr_t)p->code, PAGE_SIZE,
                 PTE_R | PTE_X | PTE_U) ||
        sv39_map(p->pgtbl, USER_BASE + PAGE_SIZE, (uintptr_t)p->data, PAGE_SIZE,
                 writable) ||
        sv39_map(p->pgtbl, USER_STACK, (uintptr_t)p->ustack, PAGE_SIZE,
                 writable) ||
        sv39_map(p->pgtbl, TRAPFRAME, (uintptr_t)p->tf, PAGE_SIZE, PTE_R | PTE_W) ||
        sv39_map(p->pgtbl, p->kstack, (uintptr_t)p->stack_page, PAGE_SIZE, PTE_R | PTE_W) ||
        sv39_map(p->pgtbl, TRAMPOLINE, (uintptr_t)trampoline, PAGE_SIZE, PTE_R | PTE_X))
        return -1;
    /* Newly copied executable bytes must be visible to instruction fetch. */
    if (!shared) __asm__ volatile("fence.i" ::: "memory");
    return 0;
}

int user_copyin(pagetable_t root, void *dst, uintptr_t src, size_t count)
{
    if (count == 0)
        return 0;
    if (src >= VA_MAX || count > VA_MAX - src)
        return -1;
    /* Validate the entire range before producing any externally visible output. */
    uintptr_t end = src + count;
    for (uintptr_t va = PAGE_ROUND_DOWN(src); va < end; va += PAGE_SIZE) {
        uint64 flags;
        if (!sv39_lookup(root, va, NULL, &flags) ||
            (flags & (PTE_U | PTE_R)) != (PTE_U | PTE_R))
            return -1;
    }
    uint8 *out = dst;
    for (size_t i = 0; i < count; ++i) {
        uintptr_t pa;
        if (!sv39_lookup(root, src + i, &pa, NULL))
            return -1; /* Owner-private tables cannot change during this copy. */
        out[i] = *(const uint8 *)pa;
    }
    return 0;
}

/* This owned-frame operation is deliberately restricted to the heap arena. */
int vm_unmappages(pagetable_t root, uintptr_t begin, size_t npages)
{
    if (!root || !npages || (begin & PAGE_MASK) || begin < USER_HEAP_START ||
        begin >= USER_HEAP_END || npages > (USER_HEAP_END - begin) / PAGE_SIZE)
        return -1;
    uintptr_t end = begin + npages * PAGE_SIZE;
    for (uintptr_t va = begin; va < end; va += PAGE_SIZE)
        if (!user_heap_page(root, va))
            return -1;
    for (uintptr_t va = begin; va < end; va += PAGE_SIZE) {
        pte_t *pte = sv39_walk(root, va, false);
        uintptr_t pa = sv39_pte_to_pa(*pte);
        *pte = 0;
        lifecycle_heap_add(heap_leaves, 1);
        sfence_vma(); /* Revoke translations before the frame can be reused. */
        page_free((void *)pa);
        sv39_prune_empty(root, va);
    }
    return 0;
}

static bool writable_user_flags(uint64 flags)
{
    return (flags & (PTE_U | PTE_R)) == (PTE_U | PTE_R) &&
           (flags & (PTE_W | PTE_COW)) != 0;
}

static bool heap_leaf(uint64 flags, uintptr_t pa)
{
    return writable_user_flags(flags) && !(flags & PTE_X) &&
           pa >= page_zone_start(PAGE_ZONE_USER) &&
           pa < page_zone_end(PAGE_ZONE_USER);
}

bool user_heap_page(pagetable_t root, uintptr_t va)
{
    uintptr_t pa;
    uint64 flags;
    return sv39_lookup(root, va, &pa, &flags) && heap_leaf(flags, pa);
}

void user_vm_clear_heap(pagetable_t root, uintptr_t begin, uintptr_t end)
{
    assert(root && !((begin | end) & PAGE_MASK) && begin >= USER_HEAP_START &&
           begin <= end && end <= USER_HEAP_END, "heap clear range");
    for (uintptr_t va = begin; va < end;) {
        uintptr_t base = va & ~(SV39_LEAF_SPAN - 1);
        uintptr_t limit = base + SV39_LEAF_SPAN;
        if (limit > end)
            limit = end;

        pte_t *table = sv39_walk(root, base, false);
        if (table != NULL) {
            unsigned first = sv39_index(0, va);
            unsigned last = first + (limit - va) / PAGE_SIZE;
            lifecycle_add(destroy_slots, last - first);
            bool revoked = false;
            for (unsigned i = first; i < last; ++i) {
                lifecycle_add(pte_visits, 1);
                pte_t entry = table[i];
                if (entry == 0)
                    continue;
                assert((entry & PTE_V) && heap_leaf(entry, sv39_pte_to_pa(entry)),
                       "heap leaf ownership");
                /* Keep the frame number in an invalid PTE until the fence.
                 * The root is inactive and only the owner can access it. */
                table[i] = entry & ~PTE_V;
                revoked = true;
            }
            if (revoked) {
                sfence_vma();
                for (unsigned i = first; i < last; ++i) {
                    lifecycle_add(pte_visits, 1);
                    pte_t entry = table[i];
                    if (entry == 0)
                        continue;
                    table[i] = 0;
                    lifecycle_heap_add(heap_leaves, 1);
                    page_free((void *)sv39_pte_to_pa(entry));
                }
                sv39_prune_empty(root, va);
            }
        }
        va = limit;
    }
}

bool user_writable(pagetable_t root, uintptr_t dst, size_t count)
{
    if (!count) return true;
    if (dst >= VA_MAX || count > VA_MAX - dst) return false;
    for (uintptr_t va = PAGE_ROUND_DOWN(dst); va < dst + count; va += PAGE_SIZE) {
        uint64 flags;
        if (!sv39_lookup(root, va, NULL, &flags) || !writable_user_flags(flags))
            return false;
    }
    return true;
}

static inline void cow_mark_leaf(pte_t *pte)
{
    pte_t entry = *pte;
    assert((entry & PTE_V) && writable_user_flags(entry) &&
           !(entry & PTE_X), "fork writable user leaf");
    *pte = (entry & ~PTE_W) | PTE_COW;
}

void user_cow_mark(pagetable_t root, uintptr_t va)
{
    pte_t *pte = sv39_walk(root, va, false);
    assert(pte, "fork writable user leaf");
    cow_mark_leaf(pte);
}

static uintptr_t leaf_span_end(uintptr_t va, uintptr_t end)
{
    uintptr_t boundary = (va & ~(SV39_LEAF_SPAN - 1)) + SV39_LEAF_SPAN;
    return boundary < end ? boundary : end;
}

int user_vm_clone_heap_span(pagetable_t dst, pagetable_t src,
                           uintptr_t begin, uintptr_t end)
{
    for (uintptr_t va = begin; va < end;) {
        uintptr_t limit = leaf_span_end(va, end);
        const pte_t *parent = sv39_walk(src, va, false);
        if (!parent)
            return -12;
        pte_t *child = sv39_walk(dst, va, true);
        if (!child)
            return -12;

        size_t pages = (limit - va) / PAGE_SIZE;
        for (size_t i = 0; i < pages; ++i) {
            lifecycle_add(clone_slots, 1);
            /* sv39_walk already counted the first slot in each table. */
            lifecycle_add(pte_visits, i != 0);
            pte_t entry = parent[i];
            if (!(entry & PTE_V) || !sv39_pte_is_leaf(entry))
                return -12;
            uintptr_t pa = sv39_pte_to_pa(entry);
            if (pa >= SV39_MAX_PA)
                return -12;
            lifecycle_add(pte_visits, i != 0);
            if (child[i] & PTE_V)
                return -12;
            child[i] = sv39_pa_to_pte(pa) | PTE_V | PTE_A | PTE_R | PTE_U | PTE_COW;
            page_retain_user((void *)pa);
        }
        va = limit;
    }
    return 0;
}

void user_vm_cow_mark_heap_span(pagetable_t root, uintptr_t begin, uintptr_t end)
{
    for (uintptr_t va = begin; va < end;) {
        uintptr_t limit = leaf_span_end(va, end);
        pte_t *first = sv39_walk(root, va, false);
        assert(first, "fork heap leaf table");
        size_t pages = (limit - va) / PAGE_SIZE;
        for (size_t i = 0; i < pages; ++i) {
            lifecycle_add(pte_visits, i != 0);
            cow_mark_leaf(&first[i]);
        }
        va = limit;
    }
}

/* The fixed-image PCB pointers own the same references represented by their leaves.
 * Heap ownership lives in the PTE. In both cases revoke translations before dropping a frame. */
static void install_user_frame(struct proc *p, uintptr_t va, pte_t *pte,
                               void *frame, uint64 flags)
{
    void *old = (void *)sv39_pte_to_pa(*pte);
    *pte = sv39_pa_to_pte((uintptr_t)frame) | flags;
    switch (va) {
    case USER_BASE: p->code = frame; break;
    case USER_BASE + PAGE_SIZE: p->data = frame; break;
    case USER_STACK: p->ustack = frame; break;
    }
    sfence_vma();
    if (old != frame) {
        if (branch_borrows(p, va))
            branch_drop_borrow(p, va);
        else
            page_free(old);
        branch_map(p, va);
    }
}

void user_replace_image(struct proc *p, void *code, void *data, void *stack)
{
    pte_t *code_pte = sv39_walk(p->pgtbl, USER_BASE, false);
    pte_t *data_pte = sv39_walk(p->pgtbl, USER_BASE + PAGE_SIZE, false);
    pte_t *stack_pte = sv39_walk(p->pgtbl, USER_STACK, false);
    assert(code_pte && (*code_pte & PTE_V) && data_pte && (*data_pte & PTE_V) &&
           stack_pte && (*stack_pte & PTE_V), "exec image leaves");
    uint64 common = PTE_V | PTE_U | PTE_R | PTE_A;
    uint64 writable = common | PTE_W | PTE_D;
    install_user_frame(p, USER_BASE, code_pte, code, common | PTE_X);
    install_user_frame(p, USER_BASE + PAGE_SIZE, data_pte, data, writable);
    install_user_frame(p, USER_STACK, stack_pte, stack, writable);
}

static void commit_cow(struct proc *p, uintptr_t va, pte_t *pte, void *frame)
{
    void *old = (void *)sv39_pte_to_pa(*pte);
    if (frame != old) {
        memcpy(frame, old, PAGE_SIZE);
        lifecycle_add(copied, PAGE_SIZE);
        lifecycle_add(cow_copy, 1);
    } else {
        lifecycle_add(cow_upgrade, 1);
    }
    uint64 flags = ((*pte & PTE_FLAGS_MASK) & ~PTE_COW) | PTE_W | PTE_A | PTE_D;
    install_user_frame(p, va, pte, frame, flags);
}

int user_cow_fault(struct proc *p, uintptr_t address)
{
    uintptr_t va = PAGE_ROUND_DOWN(address);
    pte_t *pte = sv39_walk(p->pgtbl, va, false);
    if (!pte || (*pte & (PTE_V | PTE_U | PTE_R | PTE_COW | PTE_W | PTE_X)) !=
                (PTE_V | PTE_U | PTE_R | PTE_COW))
        return -14;
    lifecycle_add(cow_fault, 1);
#ifdef LIFECYCLE_PROBE
    bool first = lifecycle_active && !p->timed_heap_write &&
                 va >= USER_HEAP_START && va < USER_HEAP_END;
    uint64 start = first ? lifecycle_time() : 0;
#endif
    void *frame = (void *)sv39_pte_to_pa(*pte);
    struct split_plan plan = {0};
    if (p->branch && branch_prepare(p, va, va + PAGE_SIZE, &plan)) return -12;
    if (branch_borrows(p, va) || page_user_refs(frame) > 1) {
        frame = page_alloc(PAGE_ZONE_USER);
        if (!frame) {
            if (p->branch) branch_cancel(&plan);
            return -12;
        }
    }
    if (p->branch) {
        branch_commit(p, &plan);
        pte = sv39_walk(p->pgtbl, va, false);
    }
    commit_cow(p, va, pte, frame);
#ifdef LIFECYCLE_PROBE
    if (first) {
        template_stats.first_write_ticks += lifecycle_time() - start;
        ++template_stats.first_writes;
        p->timed_heap_write = true;
    }
#endif
    return 0;
}

struct prepared_frame {
    struct prepared_frame *next;
    uintptr_t va;
};

/* Stage allocations in the new frames themselves. An allocation failure leaves all
 * original frame references, PTEs and bytes intact, even across a page boundary. */
int user_prepare_write(struct proc *p, uintptr_t dst, size_t count)
{
    if (!user_writable(p->pgtbl, dst, count))
        return -14;
    if (!count)
        return 0;
    struct prepared_frame *pending = NULL;
    uintptr_t start = PAGE_ROUND_DOWN(dst), end = dst + count;
    struct split_plan plan = {0};
    if (p->branch && branch_prepare(p, start, end, &plan)) return -12;
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        pte_t *pte = sv39_walk(p->pgtbl, va, false);
        if (!(*pte & PTE_COW))
            continue;
        lifecycle_add(cow_copyout, 1);
        if (!branch_borrows(p, va) && page_user_refs((void *)sv39_pte_to_pa(*pte)) == 1)
            continue;
        struct prepared_frame *frame = page_alloc(PAGE_ZONE_USER);
        if (!frame) {
            while (pending) {
                frame = pending;
                pending = frame->next;
                page_free(frame);
            }
            if (p->branch) branch_cancel(&plan);
            return -12;
        }
        *frame = (struct prepared_frame){pending, va};
        pending = frame;
    }
    if (p->branch) branch_commit(p, &plan);
    while (pending) {
        struct prepared_frame *frame = pending;
        pending = frame->next;
        commit_cow(p, frame->va, sv39_walk(p->pgtbl, frame->va, false), frame);
    }
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        pte_t *pte = sv39_walk(p->pgtbl, va, false);
        if (*pte & PTE_COW)
            commit_cow(p, va, pte, (void *)sv39_pte_to_pa(*pte));
    }
    return 0;
}

int user_copyout(struct proc *p, uintptr_t dst, const void *src, size_t count)
{
    int rc = user_prepare_write(p, dst, count);
    if (rc != 0)
        return rc;
    const uint8 *in = src;
    while (count) {
        uintptr_t pa;
        bool mapped = sv39_lookup(p->pgtbl, dst, &pa, NULL);
        assert(mapped, "prepared user write missing");
        size_t chunk = PAGE_SIZE - (dst & PAGE_MASK);
        if (chunk > count)
            chunk = count;
        memcpy((void *)pa, in, chunk);
        dst += chunk;
        in += chunk;
        count -= chunk;
    }
    return 0;
}

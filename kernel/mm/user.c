#include <kernel/lifecycle_probe.h>
#include <asm/csr.h>
#include <kernel/proc.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <kernel/heap.h>

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

bool user_heap_page(pagetable_t root, uintptr_t va)
{
    uintptr_t pa;
    uint64 flags;
    return sv39_lookup(root, va, &pa, &flags) && writable_user_flags(flags) &&
           !(flags & PTE_X) && pa >= page_zone_start(PAGE_ZONE_USER) &&
           pa < page_zone_end(PAGE_ZONE_USER);
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

void user_cow_mark(pagetable_t root, uintptr_t va)
{
    pte_t *pte = sv39_walk(root, va, false);
    assert(pte && (*pte & PTE_V) && writable_user_flags(*pte) &&
           !(*pte & PTE_X), "fork writable user leaf");
    *pte = (*pte & ~PTE_W) | PTE_COW;
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
    if (old != frame)
        page_free(old);
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
    void *frame = (void *)sv39_pte_to_pa(*pte);
    if (page_user_refs(frame) > 1) {
        frame = page_alloc(PAGE_ZONE_USER);
        if (!frame)
            return -12;
    }
    commit_cow(p, va, pte, frame);
    return 0;
}

struct prepared_frame {
    struct prepared_frame *next;
    pte_t *pte;
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
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        pte_t *pte = sv39_walk(p->pgtbl, va, false);
        if (!(*pte & PTE_COW))
            continue;
        lifecycle_add(cow_copyout, 1);
        if (page_user_refs((void *)sv39_pte_to_pa(*pte)) == 1)
            continue;
        struct prepared_frame *frame = page_alloc(PAGE_ZONE_USER);
        if (!frame) {
            while (pending) {
                frame = pending;
                pending = frame->next;
                page_free(frame);
            }
            return -12;
        }
        *frame = (struct prepared_frame){pending, pte, va};
        pending = frame;
    }
    while (pending) {
        struct prepared_frame *frame = pending;
        pending = frame->next;
        commit_cow(p, frame->va, frame->pte, frame);
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

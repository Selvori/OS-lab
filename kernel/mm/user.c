#include <kernel/proc.h>
#include <kernel/string.h>

extern char trampoline[], user_image_start[], user_image_end[];

int user_vm_create(struct proc *p)
{
    size_t image_size = (size_t)(user_image_end - user_image_start);
    if (image_size == 0 || image_size > PAGE_SIZE)
        return -1;
    for (size_t i = 0; i < image_size; ++i)
        ((uint8 *)p->code)[i] = (uint8)user_image_start[i];
    if (sv39_map(p->pgtbl, USER_BASE, (uintptr_t)p->code, PAGE_SIZE,
                 PTE_R | PTE_X | PTE_U) ||
        sv39_map(p->pgtbl, USER_BASE + PAGE_SIZE, (uintptr_t)p->data, PAGE_SIZE,
                 PTE_R | PTE_W | PTE_U) ||
        sv39_map(p->pgtbl, USER_STACK, (uintptr_t)p->ustack, PAGE_SIZE,
                 PTE_R | PTE_W | PTE_U) ||
        sv39_map(p->pgtbl, TRAPFRAME, (uintptr_t)p->tf, PAGE_SIZE, PTE_R | PTE_W) ||
        sv39_map(p->pgtbl, USER_KSTACK, (uintptr_t)p->stack_page, PAGE_SIZE, PTE_R | PTE_W) ||
        sv39_map(p->pgtbl, TRAMPOLINE, (uintptr_t)trampoline, PAGE_SIZE, PTE_R | PTE_X))
        return -1;
    /* Newly copied executable bytes must be visible to instruction fetch. */
    __asm__ volatile("fence.i" ::: "memory");
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
            return -1; /* Tables are private and immutable while a process runs. */
        out[i] = *(const uint8 *)pa;
    }
    return 0;
}

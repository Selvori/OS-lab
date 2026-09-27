#include <asm/csr.h>
#include <asm/sv39.h>
#include <kernel/page.h>

static bool pte_is_leaf(pte_t pte)
{
    return (pte & (PTE_R | PTE_W | PTE_X)) != 0;
}

pte_t *sv39_walk(pagetable_t root, uintptr_t va, bool allocate)
{
    if (root == NULL || va >= SV39_MAX_LOW_VA)
        return NULL;

    for (int level = 2; level > 0; --level) {
        pte_t *pte = &root[sv39_index((unsigned)level, va)];

        if ((*pte & PTE_V) != 0) {
            if (pte_is_leaf(*pte))
                return NULL;
            root = (pagetable_t)sv39_pte_to_pa(*pte);
            continue;
        }

        if (!allocate)
            return NULL;

        pagetable_t child = page_alloc(PAGE_ZONE_KERNEL);
        if (child == NULL)
            return NULL;

        *pte = sv39_pa_to_pte((uintptr_t)child) | PTE_V;
        root = child;
    }

    return &root[sv39_index(0, va)];
}

int sv39_map(pagetable_t root, uintptr_t va, uintptr_t pa,
             size_t size, uint64 flags)
{
    uint64 allowed = PTE_R | PTE_W | PTE_X | PTE_U | PTE_G;

    if (size == 0 || ((va | pa | size) & PAGE_MASK) != 0)
        return -1;
    if (va >= SV39_MAX_LOW_VA || size > SV39_MAX_LOW_VA - va)
        return -1;
    if (pa >= SV39_MAX_PA || size > SV39_MAX_PA - pa)
        return -1;
    if ((flags & ~allowed) != 0 ||
        ((flags & PTE_W) != 0 && (flags & PTE_R) == 0))
        return -1;

    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        pte_t *pte = sv39_walk(root, va + offset, true);
        if (pte == NULL || (*pte & PTE_V) != 0)
            return -1;

        uint64 leaf_flags = flags | PTE_V | PTE_A;
        if ((flags & PTE_W) != 0)
            leaf_flags |= PTE_D;
        *pte = sv39_pa_to_pte(pa + offset) | leaf_flags;
    }

    return 0;
}

bool sv39_lookup(pagetable_t root, uintptr_t va,
                 uintptr_t *pa, uint64 *flags)
{
    pte_t *pte = sv39_walk(root, va, false);

    if (pte == NULL || (*pte & PTE_V) == 0 || !pte_is_leaf(*pte))
        return false;

    if (pa != NULL)
        *pa = sv39_pte_to_pa(*pte) + (va & PAGE_MASK);
    if (flags != NULL)
        *flags = *pte & PTE_FLAGS_MASK;
    return true;
}

void sv39_activate(pagetable_t root)
{
    uintptr_t address = (uintptr_t)root;

    if (root == NULL || (address & PAGE_MASK) != 0 ||
        address >= SV39_MAX_PA)
        return;

    uint64 root_ppn = (address >> PAGE_SHIFT) & SATP_PPN_MASK;
    csr_write(satp, SATP_MODE_SV39 | root_ppn);
    sfence_vma();
}

uint64 sv39_read_satp(void)
{
    return csr_read(satp);
}

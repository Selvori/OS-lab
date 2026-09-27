#ifndef OSLAB_SV39_H
#define OSLAB_SV39_H

#include <kernel/page.h>
#include <kernel/types.h>

typedef uint64 pte_t;
typedef pte_t *pagetable_t;

#define PTE_V (1UL << 0)
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)
#define PTE_G (1UL << 5)
#define PTE_A (1UL << 6)
#define PTE_D (1UL << 7)
#define PTE_FLAGS_MASK 0x3ffUL

#define SV39_VPN_MASK 0x1ffUL
#define SV39_MAX_LOW_VA (1UL << 38)
#define SV39_MAX_PA (1UL << 56)
#define SATP_MODE_SV39 (8UL << 60)
#define SATP_PPN_MASK ((1UL << 44) - 1UL)

static inline unsigned sv39_index(unsigned level, uintptr_t va)
{
    return (unsigned)((va >> (PAGE_SHIFT + 9UL * level)) & SV39_VPN_MASK);
}

static inline pte_t sv39_pa_to_pte(uintptr_t pa)
{
    return ((pte_t)(pa >> PAGE_SHIFT)) << 10;
}

static inline uintptr_t sv39_pte_to_pa(pte_t pte)
{
    return (uintptr_t)((pte >> 10) << PAGE_SHIFT);
}

pte_t *sv39_walk(pagetable_t root, uintptr_t va, bool allocate);
int sv39_map(pagetable_t root, uintptr_t va, uintptr_t pa,
             size_t size, uint64 flags);
bool sv39_lookup(pagetable_t root, uintptr_t va,
                 uintptr_t *pa, uint64 *flags);
void sv39_activate(pagetable_t root);
uint64 sv39_read_satp(void);

#endif

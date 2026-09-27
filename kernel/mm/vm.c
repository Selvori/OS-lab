#include <asm/sv39.h>
#include <kernel/page.h>
#include <kernel/print.h>
#include <kernel/vm.h>
#include <platform.h>

static pagetable_t kernel_pagetable;

extern char kernel_text_start[];
extern char kernel_text_end[];
extern char kernel_rodata_start[];
extern char kernel_rodata_end[];
extern char kernel_data_start[];
extern char kernel_end[];

static void map_region(uintptr_t start, uintptr_t end, uint64 flags)
{
    if (start >= end || (start & PAGE_MASK) != 0 ||
        (end & PAGE_MASK) != 0)
        panic("invalid kernel map range");

    if (sv39_map(kernel_pagetable, start, start, end - start, flags) != 0)
        panic("kernel map failed");
}

static void verify_mapping(uintptr_t address, uint64 required,
                           uint64 forbidden)
{
    uintptr_t physical;
    uint64 flags;

    if (!sv39_lookup(kernel_pagetable, address, &physical, &flags))
        panic("kernel mapping missing");
    if (physical != address)
        panic("kernel map not identity");
    if ((flags & required) != required || (flags & forbidden) != 0)
        panic("kernel map permissions");
}

static void verify_unmapped(uintptr_t address)
{
    if (sv39_lookup(kernel_pagetable, address, NULL, NULL))
        panic("unexpected kernel mapping");
}

void kernel_vm_init(void)
{
    if (kernel_pagetable != NULL)
        panic("kernel vm initialized twice");

    uintptr_t text_start = (uintptr_t)kernel_text_start;
    uintptr_t text_end = (uintptr_t)kernel_text_end;
    uintptr_t rodata_start = (uintptr_t)kernel_rodata_start;
    uintptr_t rodata_end = (uintptr_t)kernel_rodata_end;
    uintptr_t data_start = (uintptr_t)kernel_data_start;
    uintptr_t image_end = (uintptr_t)kernel_end;

    if (text_start != PAGE_ROUND_DOWN(text_start) ||
        text_end != rodata_start || rodata_end != data_start ||
        image_end > DRAM_END)
        panic("invalid linker memory layout");

    kernel_pagetable = page_alloc(PAGE_ZONE_KERNEL);
    if (kernel_pagetable == NULL)
        panic("no root page table");

    map_region(UART_BASE, UART_BASE + PAGE_SIZE, PTE_R | PTE_W);
    map_region(PLIC_BASE, PLIC_BASE + PLIC_SIZE, PTE_R | PTE_W);
    map_region(text_start, text_end, PTE_R | PTE_X);
    map_region(rodata_start, rodata_end, PTE_R);
    map_region(data_start, DRAM_END, PTE_R | PTE_W);

    verify_mapping(UART_BASE, PTE_R | PTE_W, PTE_X | PTE_U);
    verify_mapping(PLIC_BASE, PTE_R | PTE_W, PTE_X | PTE_U);
    verify_mapping(text_start, PTE_R | PTE_X, PTE_W | PTE_U);
    verify_mapping(rodata_start, PTE_R, PTE_W | PTE_X | PTE_U);
    verify_mapping(data_start, PTE_R | PTE_W, PTE_X | PTE_U);
    verify_mapping(DRAM_END - PAGE_SIZE, PTE_R | PTE_W, PTE_X | PTE_U);
    verify_unmapped(CLINT_BASE);
    verify_unmapped(DRAM_BASE);
}

void kernel_vm_activate(void)
{
    if (kernel_pagetable == NULL)
        panic("kernel vm not initialized");
    sv39_activate(kernel_pagetable);
    if (!kernel_vm_active())
        panic("failed to activate sv39");
}

pagetable_t kernel_vm_root(void)
{
    return kernel_pagetable;
}

bool kernel_vm_active(void)
{
    uint64 satp = sv39_read_satp();
    uint64 expected_ppn = ((uintptr_t)kernel_pagetable >> PAGE_SHIFT) &
                          SATP_PPN_MASK;
    return (satp >> 60) == 8 && (satp & SATP_PPN_MASK) == expected_ppn;
}

#include <kernel/arch.h>
#include <kernel/page.h>
#include <kernel/print.h>
#include <kernel/vm.h>
#include <platform.h>

/* 位于 BSS，_entry 会在冷启动时将它清零。 */
static uint32 started;

void main(void)
{
    int cpuid = (int)arch_cpu_id();

    if (arch_is_boot_cpu()) {
        print_init();
        printf("cpu %d is booting!\n", cpuid);

        page_allocator_init();
        printf("memory: kernel %p-%p, %d free pages\n",
               (void *)page_zone_start(PAGE_ZONE_KERNEL),
               (void *)page_zone_end(PAGE_ZONE_KERNEL),
               (int)page_free_count(PAGE_ZONE_KERNEL));
        printf("memory: user   %p-%p, %d free pages\n",
               (void *)page_zone_start(PAGE_ZONE_USER),
               (void *)page_zone_end(PAGE_ZONE_USER),
               (int)page_free_count(PAGE_ZONE_USER));

        kernel_vm_init();
        kernel_vm_activate();
        printf("cpu %d enabled Sv39 with root %p\n", cpuid,
               (void *)kernel_vm_root());

        /* arch_start_cpu() 接收内核 CPU 编号。 */
        for (int cpu = 0; cpu < NCPU; ++cpu) {
            if (cpu == cpuid)
                continue;

            if (arch_start_cpu((uint64)cpu) != 0)
                panic("start_cpu failed");
        }

        /* 发布页表和其他启动核初始化结果。 */
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) == 0) {
        }

        kernel_vm_activate();
        printf("cpu %d enabled Sv39 with root %p\n", cpuid,
               (void *)kernel_vm_root());
    }

    /* 当前实验没有调度器，完成初始化后停在 wfi 循环中。 */
    arch_park();
}

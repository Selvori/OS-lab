#include <kernel/arch.h>
#include <kernel/print.h>
#include <platform.h>

// TODO(lab-1): 主核初始化并启动其他核，以原子操作同步；每核打印一次。

/*
 * 位于 BSS，_entry 会在冷启动时将它清零。
 */ 
static uint32 started;

void main(void)
{
    int cpuid = (int)arch_cpu_id();

    if (arch_is_boot_cpu()) {
        /*
         * UART 和打印锁只能由主核初始化一次。
         */
        print_init();

        printf("cpu %d is booting!\n", cpuid);

        /*
         * arch_start_cpu() 接收内核 CPU 编号。
         * 它会在内部加上 HART_FIRST，转换成硬件 hartid。
         */
        for (int cpu = 0; cpu < NCPU; ++cpu) {
            if (cpu == cpuid)
                continue;

            if (arch_start_cpu((uint64)cpu) != 0)
                panic("start_cpu failed");
        }

        /*
         * 发布初始化结果，允许从核继续执行。
         */
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    } else {
        /*
         * 从核可能在主核完成 print_init() 前进入 main，
         * 因此必须先等待。
         */
        while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) == 0) {
        }

        printf("cpu %d is booting!\n", cpuid);
    }

    /*
     * 当前实验没有调度器。完成启动信息输出后，
     * 让每个 CPU 停在 wfi 循环中。
     */
    arch_park();
}

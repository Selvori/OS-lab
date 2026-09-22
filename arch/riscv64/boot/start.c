/* 汇编已建立栈并保存 hartid。 */
#include <kernel/arch.h>
#include <kernel/print.h>
extern void main(void);

// TODO(lab-1): 整理本核初始状态，再进入 main。
void start(uint64 hartid, uint64 opaque)
{
    (void)hartid;
    (void)opaque;
    
    arch_early_init();
    main();

    /* main 理论上不会返回*/
    arch_park();
}

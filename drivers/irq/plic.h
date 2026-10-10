#ifndef OSLAB_PLIC_H
#define OSLAB_PLIC_H
#include <kernel/types.h>

/* 全局优先级只由启动核设置；各核仅操作自己的 S-mode context。 */
void plic_init(void);
void plic_inithart(void);
uint32 plic_claim(void);
void plic_complete(uint32 irq);
#endif

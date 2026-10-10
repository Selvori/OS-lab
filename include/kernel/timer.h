#ifndef OSLAB_TIMER_H
#define OSLAB_TIMER_H
#include <kernel/types.h>

#define TIMER_HZ 100UL
/* 每核在 SIE=0、正式陷阱入口安装后调用一次；不自行开启中断。 */
void timer_inithart(void);
void timer_interrupt_handler(void);
/* 已处理的中断次数，不是墙上时间；允许跨核读取。 */
uint64 timer_ticks(uint64 cpu);
#endif

#ifndef OSLAB_TRAP_H
#define OSLAB_TRAP_H

struct kernel_trapframe;

/* 每个 hart 在页表和控制台就绪后调用；此函数不打开中断。 */
void trap_kernel_inithart(void);
/* 仅由汇编入口调用；参数指向被打断执行流在本核栈上的现场。 */
void trap_kernel_handler(struct kernel_trapframe *frame);

#endif

# Lab 6：进程管理与时钟抢占调度

从 Lab 5 的单进程演进为16槽PCB数组、启动核运行的多进程轮转调度器。实现深拷贝 `fork`、阻塞 `wait/sleep`、僵尸退出与回收、孤儿收养和用户态时钟抢占。0号进程完成演示后持续睡眠，内核继续处理UART与时钟。

详细任务、实现过程和操作系统知识见 [Lab 6实验报告](docs/lab-6-report.md)。规格入口为 [design.yaml](design.yaml)、[进程合同](modules/process.yaml)、[进程内存合同](modules/process-memory.yaml)、[Lab 6增量](patches/lab6.yaml)。实验依据是课程 `lab-6.pptx` 全8页；容量、ABI编号及孤儿策略是本项目的实现选择。

## 实现

- PCB状态为 UNUSED/RUNNABLE/RUNNING/SLEEPING/ZOMBIE，另有仅用于未发布分配的 USED 状态；PID与数组槽分离。
- 创建只发布RUNNABLE，由调度器在启动栈上循环选择；`swtch` 保存ra/sp/s0-s11，恢复各进程的内核继续点。
- 每进程拥有私有代码、数据、单页用户栈、trapframe、页表、内核栈和堆页面；每个内核栈有唯一别名与guard页。
- fork深拷贝用户帧和堆空闲链表，子进程saved a0=0，父进程获得子PID；失败完整回滚。
- wait阻塞在父PCB通道，exit保留僵尸并唤醒父进程；由其他栈回收资源。孤儿由PID0收养。
- sleep阻塞在owner tick通道；到期变为RUNNABLE。用户时钟中断处理后yield，CPU密集进程也会被轮换。

## 系统调用

沿用a7编号、a0-a2参数、a0返回。原write/getpid/ticks/exit/mmap/munmap为1–6。

| 编号 | 接口 | 结果 |
| --- | --- | --- |
| 7 | fork() | 父返回子PID，子返回0；-11容量不足，-12资源不足 |
| 8 | wait(status_va) | 返回一个子PID，向可写用户地址写int32状态；0忽略状态；-10无子进程，-14非法指针 |
| 9 | sleep(ticks) | 100Hz owner时钟的已处理节拍；0让出CPU；超过0x7fffffff返回-22 |
| 10 | yield() | 主动让出CPU，恢复后返回0 |

exit(4)使子进程进入僵尸态；PID0调用exit返回-1并继续运行。用户故障使子进程以-1退出；PID0故障属于致命错误。

## 构建与验收

```bash
cd ~/Work/Seele
make CONFIG=riscv64-qemu-virt-sbi build
make CONFIG=riscv64-qemu-virt-sbi run
# Ctrl-A，松开后按 X 退出
python3 tools/test_lab6.py
python3 tools/test_lab6.py --extended
make CONFIG=riscv64-visionfive2-uboot build image
```

本分支使用 `test_lab6.py`；Lab4/5脚本保留历史单进程退出预期，分别在对应历史分支运行。扩展验收会临时编辑init/main/page/proc，测试期间避免同时编辑这些文件；finally恢复原字节、核对SHA-256并重建。

正式程序输出示例：

```text
HEAP PASS: zero, split, merge, reuse, errors, 256 cycles
USER PASS: 4096 round trips, registers, stack, data, errors, timer
FORK/WAIT PASS: independent data, stack, heap, status, errors
SLEEP PASS: deadline and idle wakeup
CPU child: begin loop without syscalls
PREEMPT PASS: parent ran while CPU child busy
CPU child: registers and loop result preserved
LIFECYCLE PASS: 32 fork/wait cycles
LAB6 PASS: init pid 0 remains alive
```

![QEMU真实进程管理运行画面](pictures/lab6-process-management.png)

## 已验证结果

- 正式用户程序：原256堆循环、4096寄存器往返、fork隔离、退出状态、睡眠、2亿轮无syscall计算期间抢占、32轮fork/wait。
- 扩展：先exit/先wait、存活与僵尸孤儿、两次无条件fork产生4进程、错峰睡眠、跨页状态、16槽耗尽与复用、yield。
- 两个CPU密集子进程保持全部非计数整数寄存器及栈，临时计数证明多次抢占；5类用户故障转僵尸且父进程回收后继续运行。
- fork全部16个页分配失败点、128节点完全耗尽与部分链表复制失败、深拷贝逐字节/权限/物理独立性、跨页copyout全范围验证与回收计数通过。
- 用户等待和计算期间UART正常；各场景完成后5777字节逐字节回显；owner-only UART路由通过。
- 两平台严格全量构建通过，入口0x80200000/0x40200000；VF2 FIT识别通过，payload为24972字节。本次未上板。
- 精选日志、恢复哈希与结果见 [docs/lab-6-evidence](docs/lab-6-evidence)。

## 架构与范围

```text
main → proc_make_first → proc_scheduler
  → 选择RUNNABLE → swtch(scheduler, process) → trap_user_return → U-mode
  → ecall / timer → trampoline → 用户陷阱处理
  → fork / wait / sleep / exit / yield → swtch(process, scheduler)
```

所有用户进程固定在启动核运行，其他核服务设备；PCB/父子关系/睡眠条件在owner关中断期间修改，因此没有跨核进程表竞争。内核处理期间不可抢占，用户态时钟以1个tick为轮换单位。支持SMP用户并行、进程迁移或内核抢占时，需要补进程锁、等待条件锁和远程TLB失效。当前使用立即深拷贝，保留exec、COW、文件系统和浮点/向量上下文给后续扩展。

前阶段：[Lab 5 README](docs/lab-5-baseline.md)、[Lab 5详细报告](docs/lab-5-report.md)。

# 进程创建与回收研究

起点为 Lab 9 的 `f43d455a584df6d3102af4e87479a3794da2396a`。QEMU Lab 9 正式回归通过，包含磁盘 Shell、描述符与打开对象上限、共享偏移、exec 成功与失败、520 字节重启读回及内嵌 Lab 6 回退。

在同一套 QEMU 用户负载上比较深拷贝与独立页表的标准写时复制，验证生命周期、失败回滚和资源余额。调度沿用启动核独占模型，研究范围到预热父进程对照。

## 深拷贝的测量口径

~~~sh
python3 tools/test_lab9.py
python3 tools/bench_lifecycle.py --output build/process-lifecycle/deep-base.json
~~~

QEMU 5.1.0 virt、128 MiB、双 hart。用户调度仍由启动核独占。内核用 GCC 11.4.0 的 \`-O2\`，用户程序用 \`-Os\`。\`rdtime\` 使用 10 MHz timebase，每单位 100 ns。空读逐次记录，100 Hz 调度 tick 不用于换算。独立构建目录分别运行 \`LIFECYCLE=time\` 和 \`LIFECYCLE=counts\`；正式构建无研究 syscall 或计数逻辑。计时只报告 time 构建；counts 记录事件、计时值保留作诊断。

创建区间包围 \`proc_fork\`；回收计时只包围 wait 内的 \`proc_destroy\`；整轮由用户态在 fork 前及 wait 返回后读 time。初始化包含 mmap 与全工作集填充。批次包含测量控制 syscall、资源及父进程数据校验，日志输出在批次之后。

复制字节指 fork 逐页复制的用户帧或 COW 分离时复制的有效内容。exec 搬运单列；清零及释放填充不计为有效复制。页池分配/归还记录实际成功事件；峰值是相对当轮开始的最大额外在用页。页表页是内核页的子集。PTE 探查统计 Sv39 walk 和私有表回收访问，包含缺失层检查。clone/destroy 槽计数仅为堆扫描，exec 清理旧堆也计入 destroy 槽。余额在每轮末和工作集解除映射后核验。

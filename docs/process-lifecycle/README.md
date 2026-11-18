# 进程创建与回收研究

[后续批量回收实验、数据与复跑](batch-reclaim/README.md)

起点为 Lab 9 的 `f43d455a584df6d3102af4e87479a3794da2396a`。QEMU Lab 9 正式回归通过，包含磁盘 Shell、描述符与打开对象上限、共享偏移、exec 成功与失败、520 字节重启读回及内嵌 Lab 6 回退。

在同一套 QEMU 用户负载上比较深拷贝与独立页表的标准写时复制，验证生命周期、失败回滚和资源余额。调度沿用启动核独占模型，研究范围到预热父进程对照。见[预热基准与比较](comparison.md)。

## 深拷贝的测量口径

~~~sh
python3 tools/test_lab9.py
python3 tools/bench_lifecycle.py --output build/process-lifecycle/deep-base.json
~~~

QEMU 5.1.0 virt、128 MiB、双 hart。用户调度仍由启动核独占。内核用 GCC 11.4.0 的 \`-O2\`，用户程序用 \`-Os\`。\`rdtime\` 使用 10 MHz timebase，每单位 100 ns。空读逐次记录，100 Hz 调度 tick 不用于换算。独立构建目录分别运行 \`LIFECYCLE=time\` 和 \`LIFECYCLE=counts\`；正式构建无研究 syscall 或计数逻辑。计时只报告 time 构建；counts 记录事件、计时值保留作诊断。

创建区间包围 \`proc_fork\`；回收计时只包围 wait 内的 \`proc_destroy\`；整轮由用户态在 fork 前及 wait 返回后读 time。初始化包含 mmap 与全工作集填充。批次包含测量控制 syscall、资源及父进程数据校验，日志输出在批次之后。

复制字节指 fork 逐页复制的用户帧或 COW 分离时复制的有效内容。exec 搬运单列；清零及释放填充不计为有效复制。页池分配/归还记录实际成功事件；峰值是相对当轮开始的最大额外在用页。页表页是内核页的子集。PTE 探查统计 Sv39 walk 和私有表回收访问，包含缺失层检查。clone/destroy 槽计数仅为堆扫描，exec 清理旧堆也计入 destroy 槽。余额在每轮末和工作集解除映射后核验。

深拷贝复现提交为 `aaf1420`。初始测量 [原始 JSON](deep-base.json) 含每模式每场景 2 轮预热及 3 组 × 12 轮正式样本。空堆 fork 复制 12,288 字节，1 MiB 堆 fork 复制 1,060,864 字节。各轮 clone 探查 4096 槽，普通销毁探查 4096 槽，fork/exec/wait 合计销毁探查 8192 槽。两种模式均逐轮核对资源余额，正常退出 QEMU。前置 [Lab 9 验收 JSON](lab9-baseline.json) 保留 Shell 全路径和回退结果。

## 标准 COW

父子分别持有独立页表、trapframe、内核栈及堆空闲链表。子表建立成功后才把父映射的可写权限改为 Sv39 RSW 中的 COW 标志，刷新本地 TLB，再发布子进程。RX 代码共享但保持不可写。用户写缺页分配并复制，独占者原地恢复写权限，再重试原指令。跨页内核代写先预验范围和暂存所有新帧，失败时原 PTE、引用和字节原样保留。exec 直接在三个新用户帧准备代码、零数据和参数栈，成功后才替换旧映射，保留进程身份和文件状态。

QEMU Lab 9 回归通过。临时白盒断言检查 fork 后代码/数据/栈及首个已映射堆帧实际共享、引用数至少为二、权限与内核资源独立，验证后按原字节恢复并重建复验。计数试跑的 1 MiB 堆、4 页写场景观察到 4 次用户写缺页、1 次 wait 内核代写，合计复制 20,480 字节。空堆场景仅观察到 wait 写入状态所触发的分离。引用最后归还时才填充并放回页池。

## 所有权与回归

[资源表](ownership.md) 对照了帧、页表、trapframe、内核栈、堆节点、文件、cwd 和 PCB 的获取、转移、销毁与失败责任。

~~~sh
python3 tools/test_lifecycle.py
~~~

套件在临时磁盘上运行 8 轮父子/兄弟/多代隔离、首次分离、独占升级、部分解除映射、跨页 wait/read、坏地址、跨页第二次分配失败、store OOM、exec 三个分配失败点及 RX 写保护。再复用 Lab 5 的 1024 步位图模型、4096 页整堆、128 节点及 8 个 mmap 分配点，复用 Lab 6 的收养/抢占/16 槽/8 个 fork 分配点与 Lab 9 的磁盘正式回归。跨页 OOM 时断言旧 PA、权限、引用与数据均保留，磁盘读取失败不推进共享偏移，wait 可重试回收同一僵尸。

第一次注入 Lab 5 模型时，外借节点的临时 PCB 未设置当前 owner，启动核为 1 时探针触发断言。给临时 PCB 继承真实 owner 后全套通过。历史脚本的临时源码按原字节恢复，并用 SHA-256 核对。QEMU 运行磁盘 Shell 及独立程序，VF2 ELF/FIT 成功构建。入口分别为 0x80200000、0x40200000，两版正式 ELF 均无探针符号。

[完整结果](regression.json) · [用户场景串口](lifecycle-cases-serial.log) · [白盒串口](probe-serial.log) · [FIT](vf2-fit.txt)

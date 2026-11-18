# 关闭模板后的窗口提前回收

窗口提前回收让仍存活的分支在覆盖 8 MiB 堆后少占 2048 个用户页。开放期主场景的配对相对差中位数为 +0.04%，95% 区间上界为 +2.71%，满足预设的 5% 预算。关闭后触发回收的末次写承担了释放成本，四个大窗口的末次写中位数为 1.53–1.65 ms。保留 `freeze(2)` 作为可选策略，优先评估需要节省驻留内存且能容忍毫秒级写延迟的负载。 [完整结果与配对样本汇总](summary.md)

## 版本与范围

研究分支为 `research/template-window-reclaim`，起点 `c034ec2`，实现与实测提交 `7bfead210d708d31cc1379f3c0ed6a77f4c77c7e`。普通 `fork` 使用私有页表和批量 COW。`freeze(0)` 选择共享表逐项持有，`freeze(1)` 选择旧模板持有，`freeze(2)` 在旧模板持有基础上启用窗口提前回收。基线在原提交复跑旧专项，三路长寿分支全覆盖后为 2048/4096/4096 个额外用户页。 [基线 JSON](baseline/check.json) · [基线串口](baseline/template-check-serial.log)

机制覆盖单用户调度 owner、匿名堆和 4 KiB 叶页。用户地址空间由启动 hart 串行管理，内核在 SIE=0 下修改 PTE，处理器使用独立内核根；固定映像仍用普通引用。所有动态专项和性能数据来自 Ubuntu 22.04.5 VM 中的 QEMU 5.1.0 virt，128 MiB、两 hart、10 MHz `time`、GCC 11.4.0。VisionFive 2 验证到 ELF/FIT 构建与元数据检查，板端运行待单独验证。 [回归及构建结果](regression/results.json) · [FIT 元数据](regression/vf2-fit.txt)

## 所有权与释放顺序

| 状态 | 原始堆帧引用与操作 |
| --- | --- |
| 开放 | 模板为每个原始映射拥有一份帧引用，创建者和普通 COW 子进程可另有引用。各分支持有模板对象引用，可共享末级表，或在复制表后继续借用原始帧。 |
| 关闭 | 撤销派生句柄。没有分支对象引用时按原路径销毁。仍有分支时在进程表中定位同一模板的分支，扫描存在的末级表，只计算有效且未标记私有的堆槽。 |
| 已关闭且被借用 | 分支维护每窗有效原始借用 PTE 数，模板维护每窗借用数非零的分支数量。任一分支在窗口中仍有一个有效借用 PTE，就保留原窗口所有帧引用。 |
| 窗口释放 | 扫描时已无借用或末个借用者归零后，清除模板中该窗口的堆 PTE 并逐项放弃帧引用。帧引用降到零时，分配器归还实际物理页。已释放状态用位图标记。 |
| 对象销毁 | 分支对象照常在 exec 或 wait 脱离时释放模板对象引用。最后一个对象引用消失后销毁模板，已清除的堆 PTE 不参与二次减引用。 |

`kernel/proc/proc.c` 负责句柄关闭、进程表扫描及对象生命周期，断言扫描到的分支数等于对象引用减去关闭中的句柄。`kernel/mm/template.c` 负责借用转换、释放窗口和共享页表生命周期。两个计数数组分别位于现有的模板 PCB 和分支元数据页，关闭无需额外分配。冻结、派生或分裂在资源不足时使用原有失败返回和回滚；失败的操作保持账本与映射。

首次用户写或内核代写在分配、复制及发布新 PTE 后执行本地 `sfence.vma`，再报告原始借用消失。局部撤映射清叶后走同一钩子，重映射成功仅把新叶登记为私有。跨页代写先准备全部页表及帧，任何分配失败都发生在借用账本变更之前。exec 和 wait 先撤目录链接并执行 fence，再一次放弃该分支在窗口内的剩余借用。exit 留存僵尸地址空间至 wait，窗口在实际撤销该进程翻译时更新。

模板根从未激活。其共享 L0 表的存储随对象引用延续，直到各分支目录链接撤销且对象销毁。关闭期间每个窗口的借用者数等于该窗仍有借用的分支数，各分支借用不同原始页也能各自钉住整窗。混合首表由分支私有，扫描、清除与减引用都跳过代码、数据和空位前缀的固定槽。

## 专项与回归

- 三种模板策略通过原专项。分别测试冻结与派生的前八个分配失败位置、八分支隔离和内存压力、只读代码保护、写缺页 OOM、exec 三个分配失败点及成功替换。用户故障日志对应专项预期的 RX 或 OOM。 [专项原始结果](check.json) · [串口](serial/window-check-00-serial.log)
- 新策略验证单分支及双分支、先写后关与先关后写、同窗不同末页、不同窗口、稀疏洞、同址重映射、exec 后分支仍存活的采样、exit 后 wait，以及创建者独立引用仍保护原始值。跨两张表的 `munmap` 两处、`mmap` 三处、文件代写四处注入失败，核对字节、余额和新增释放计数。成功代写精确释放两个窗口的两个原始引用。 [专项串口](serial/window-check-00-serial.log)
- 独立审计在五个窗口各留一页，覆盖其余 2043 页后两版均多占 2043 页。再逐个覆盖五个末页，相对采样起点的旧版增量依次为 2044/2045/2046/2047/2048，新版为 1535/1024/513/2/0。分支保持活跃并逐页读回，最终资源余额恢复。 [独立串口](independent/page-pool-serial.log) · [来宾探针](independent/waudit.c) · [执行脚本](independent/run.py)
- 生命周期专项 8 轮、Lab 5 的 1024 步匿名堆模型与 8 个分配故障点、Lab 6 的抢占、进程槽与 UART、Lab 9 的磁盘 Shell 与重启持久化通过。历史探针源码逐字节恢复，正式 QEMU ELF 入口 `0x80200000`，VF2 ELF/FIT 入口 `0x40200000`。 [回归 JSON](regression/results.json) · [生命周期串口](regression/lifecycle-cases-serial.log)
- 试跑发现主机解析器把 `WP PASS` 当作数字行。另一次试跑在检查时首次写入来宾栈，快照早于 COW 分配，产生错误的余额差；比较前补做预热快照。两次试跑单独归档，正式样本与专项均已重跑。 [解析失败](failures/parser.json) · [预热失败](failures/rollback.json)

## 复跑

从 Ubuntu 仓库根目录执行，使用新的输出目录。采样把故障注入、操作计数和计时分开，同一模式内两种策略共用内核 ELF 和用户程序。`tbench` 的 2/3 对应 `freeze(1/2)`，`wpin` 直接接受 1/2。`rdtime` 每 tick 为 100 ns，`time` 的额外页峰值字段为 0，因为该模式关闭累加计数。`counts` 保留峰值数据。相位数值单位均为 tick，驻留单位为 4 KiB 页，原始 JSON 还记录版本和各二进制 SHA-256。串口归档统一为 UTF-8 和 LF。

~~~sh
python3 tools/bench_window_reclaim.py build/window-replay check counts time
python3 tools/report_window_reclaim.py build/window-replay
python3 tools/test_lifecycle.py --output build/window-regression-replay
make CONFIG=riscv64-qemu-virt-sbi build
make CONFIG=riscv64-visionfive2-uboot build image

mkdir -p build/independent-window-audit
cp docs/process-lifecycle/template-window-reclaim/independent/{waudit.c,run.py} build/independent-window-audit/
python3 build/independent-window-audit/run.py
~~~

[计时 JSON](time.json) · [计数 JSON](counts.json) · [首个配对串口](serial/window-time-00-serial.log) · [汇总](summary.md)

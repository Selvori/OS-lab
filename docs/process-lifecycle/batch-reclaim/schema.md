# 原始样本格式

文件为 UTF-8 JSON，`schema_version=2`。v1 深拷贝与 COW 数据仍可由 `tools/compare_lifecycle.py` 读取。

| 字段 | 内容 |
| --- | --- |
| `commit, variant, dirty` | 被测 Git 提交、实验标签、被跟踪文件的修改状态 |
| `suite, runs, batches, warmup_runs` | 场景矩阵名、每批样本数、正式批数、每场景一次预热的轮数 |
| `host, qemu, compiler, platform, kernel_flags, timebase_hz` | 采集环境与计量配置 |
| `artifacts[mode]` | 内核 ELF、sh、lifebench、lifejob 的 SHA-256 |
| `cases[]` | 每个 `mode / pages / writes / stride / job` 对应一个场景。页大小为 4096 B，stride 的单位为页 |
| `warmup, samples[]` | 一批包含 `rounds[]`、`init`、`batch`、16 次背靠背时钟差值 `time_read` 与 `last_round_tables` |
| `last_round_tables.reap/exec` | 该批最后一轮按末级表覆盖窗口编号记录的判空次数，两个阶段各有 9 项 |
| `median` | 采集时缓存的正式样本中位数。离线程序用原始轮次重算 |
| `failures[]` | 超时、异常和断言等采集失败的异常字符串。失败保留已完成批次，离线对照拒绝不完整数据 |

`rounds[]` 的时间字段为 `elapsed / fork_time / reap_time / wait_sleep_time / exec_heap_time`，单位为 RISC-V time tick，10 MHz 下 10000 tick = 1 ms。用户态的 `elapsed` 从 fork 前测至 wait 返回。计数构建的时间字段仅供调试。初始化和 batch 也用同单位，batch 计入样本前后的探针及父进程数据检查。

`copied, exec_bytes` 的单位为字节。页分配/归还/峰值字段以页为单位。 `pte_visits` 为整轮实际 PTE 探查，含两版共有的走查与批量版清除和释放扫描。 `destroy_slots` 只计销毁时对候选堆叶的首遍访问，旧版逐槽探查，批量版跳过未建的末级表。 `heap_leaves` 每撤销一个堆叶累计一次用户帧引用释放，`leaf_checks / leaf_reads / upper_checks / upper_reads / heap_table_free` 仅统计堆撤映射中的剪枝操作。 `table_free`、页池回收与 COW 事件覆盖整轮。具体作用域见[测量口径](measurement.md)。

资源字段 `user_left / kernel_left / nodes_left / slots_used / files_used` 是当轮结束时的余额。用户程序每轮同工作集准备后比较余额，整批释放父堆后同准备前比较，并逐页检查父数据。离线重算检查同场景余额稳定、用户/内核/表页分配和释放成对、逐表统计求和一致、两版用户程序摘要和采样配置完全一致。

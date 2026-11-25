# 跨核原帧归还与短任务计时

## 关闭时的实际页表与物理页池

创建者映射四页。两个 spawn 子进程各自保留 0x3000 和 0x200000，撤去 0x4000 和 0x400000，然后持续读取保留页。首窗中的两个末级表物理地址不同；在 0x200000，两个子进程链接同一张末级表。两个子进程同时读一个原帧时，借用没有增加该帧的引用计数。

创建者撤去全部四页后调用 release。断在 template_close 的第一条指令时，另一 hart 的用户 PC 落在 read_held，其 satp 与获派的子进程页表匹配。关闭前、关闭后和两次 wait 后均从宿主暂停 QEMU，读取来宾 RAM 的真实页表、user_refs 和物理页池空闲链表。两条分支在关闭后继续读取各自保留的原页，通过退出值与 PID 校验。

| 四个原始帧按上述虚拟地址顺序排列为 0x3000、0x4000、0x200000、0x400000 | 开放时引用 | 创建者撤映射并关闭后 | 关闭时已还池 | 两支回收后已还池 |
| --- | --- | --- | ---: | ---: |
| 基线策略 9，模板分支单核运行 | 2, 2, 2, 2 | 1, 0, 1, 0 | 2 | 4 |
| 并行策略 1 | 2, 2, 2, 2 | 1, 1, 1, 1 | 0 | 4 |
| 并行策略 9 | 2, 2, 2, 2 | 1, 0, 1, 0 | 2 | 4 |

并行版的开放及关闭后快照均捕获了两个处于 U 态的读者，实际根页表各异；旧版只捕获启动 hart 上的一条分支。每行给出的回池数量均与原 PFN 是否位于空闲链表逐一核对。跨核交回要求工作 hart 在切回内核根后清理自己的翻译；RISC‑V 规定 SFENCE.VMA 的作用域是执行它的 hart。[基线物理记录](original-9/physical.json) · [策略 1 物理记录](parallel-1/physical.json) · [策略 9 物理记录](parallel-9/physical.json) · [策略 9 关闭现场](parallel-9/at-close.txt) · [RISC‑V Supervisor ISA](https://docs.riscv.org/reference/isa/priv/supervisor.html)

## 工作量较小时

与主试验复用完全相同的 QEMU 内核、磁盘及 tdbench。每个子进程均扫描 8 MiB、私写两页并撤两页；变化项是每分支的整数迭代次数。两条子进程按 PID 核对校验和。每种配置四次交替顺序的独立开机，每次对每一负载预热一轮，正式三轮取中位。下表为开机级中位数；百分比先在同一对开机内计算，再取中位。

| 策略 | 每分支整数迭代 | 第一次 spawn 到两个 wait（基线 → 并行，ms） | 配对变化 | 到排空查询结束（基线 → 并行，ms） | 配对变化 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 1.86 → 1.87 | +1.0% | 1.97 → 1.98 | −0.8% |
| 9 | 1 | 1.61 → 1.73 | +8.8% | 26.03 → 26.07 | +0.2% |
| 1 | 2,000,000 | 7.04 → 4.47 | −35.5% | 7.14 → 4.62 | −34.2% |
| 9 | 2,000,000 | 6.73 → 4.30 | −37.1% | 31.43 → 28.93 | −7.8% |

计时从第一次 spawn 前开始。模板冻结和 8 MiB 的预填充在计时前完成，最后一次 wait 之后的排空包含来宾主动让出与队列查询。计时过程中创建者还持有原帧。额外整数运算接近零时，两个策略的含排空改善都在四对的波动范围内。两百万次时四对均更快，策略 9 的后台回收占据了总耗时的大部分。四组数据用于比较这些工作点。[开机级汇总](summary.json) · [完整原始样本](granularity.json.gz)

## 复跑

从 Ubuntu 内核仓库根目录执行。先按上一级报告准备完成签署代码的 time 运行，使 base 指向其清单记录的基线 ELF，timing 指向该次正式 time 输出目录。

~~~sh
proof=docs/process-lifecycle/dual-hart-template-branches/independent
out=build/template-branch-independent-replay
timing=build/template-dual-hart/formal-time
base=build/template-dual-hart/baseline-9e3a653.elf
mkdir -p "$out"
riscv64-linux-gnu-gcc -Iinclude -Iuser -Itools -std=c11 -Os -Wall -Wextra -Werror \
  -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -mno-relax \
  -march=rv64imac -mabi=lp64 -nostdlib -static -Wl,--build-id=none,--no-relax,-T,user/program.ld \
  user/start.S user/lib.c kernel/lib/string.c "$proof/held.c" -o "$out/held.elf"
riscv64-linux-gnu-objcopy -O binary "$out/held.elf" "$out/held.bin"
python3 tools/mkfs.py "$out/disk.img" --program sh "$timing/apps-build/apps/sh.bin" --program held "$out/held.bin"
python3 "$proof/check.py" "$base" "$out" "$out/original-9" 9
python3 "$proof/check.py" "$timing/parallel-time/kernel.elf" "$out" "$out/parallel-1" 1 --branch-parallel
python3 "$proof/check.py" "$timing/parallel-time/kernel.elf" "$out" "$out/parallel-9" 9 --branch-parallel
python3 "$proof/granularity.py" "$timing" "$base" "$out/small"
~~~

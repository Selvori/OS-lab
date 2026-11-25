# 最后一个模板分支交回后的有界回收

8 MiB 模板在两条分支仍存活时关闭。两支随后退出，父进程 wait，再主动 yield 到队列排空。双核上的旧 freeze(9) 在短任务的最后一次 wait 后需要 22.04 ms，新版本需要 3.70 ms。两次 wait 均已返回时仍有一个后台作业。每次 spawn 对借用堆帧的引用递增为零。[开机级汇总](summary.json) · [版本与镜像摘要](versions.json)

## 生命周期与调度

启动 hart 独占页表修改。工作 hart 在交回用户上下文前切到内核页表并刷新本核翻译。每个仍挂接的分支保留一个模板引用。最后一支在 wait 销毁旧进程或 exec 销毁旧堆时撤销页表链接，执行地址翻译失效并注销分支。句柄已经关闭，在此时对借用计数归零的观察足以证明模板原帧再无分支读者。创建者的映射继续持有独立引用。

旧实现只在关闭时就没有分支的情况下使用终态释放。新实现允许在最后一支解除挂接后进入同一路径。每个仍有原帧的 64 位字都属于 blocked 或 ready。最后一次解除挂接只合并九个窗口的字掩码，保留部分已释放字的剩余 held 位。已入队的确认项按原位置出队，废弃的局部扫描到此结束。释放器按 held 位拆批处理，旧翻译已经在交回路径失效。wait 和 exec 不遍历整堆，也不在调用者的栈上执行这批后台减引用。

若交回者尚未为另一个作业让出时间，后续显式 yield 可让调度器继续处理有界批次。调度器在 100 µs 预算到达后停止开始新批，遇到本地可运行竞争者时恢复通常的选择。队列沿用全局轮换，每批上限为 32 步确认、8 次减引用及一次析构。运行时间随其他可运行进程、主动让出机会及页数变化。双核派发用 DUAL_HART_USERS=1 打开，研究分支上的释放逻辑由 freeze(9) 选择。[单分支交回与回池验证](checks/physical.json) · [多作业证据](checks/exec-and-overlap.txt)

## 8 MiB 两条兄弟分支

对照与候选均已启用 U 态双 hart。旧内核来自 a09d365 上一轮已核对的计时 ELF，新内核代码提交为 130be23。QEMU 5.1.0、两个 hart、多线程 TCG、相同来宾磁盘和 tdbench。程序在开始计时前已填充并冻结 2048 页。每个子进程两遍核对堆数据，私写两页，撤映射两页，并完成指定整数迭代。每个 PID 的结果在宿主独立核对。四对交替顺序开机，每次每种负载一轮预热，三轮正式取中位。表内时长为四次开机的中位，变化比例先在每对开机中计算。各列独立取中位。[全部原始计时](paired.json)

| 每分支整数迭代 | 首次 spawn 到两次 wait，旧 → 新，ms | 最后一次 wait 后排空，旧 → 新，ms | 首次 spawn 到排空，旧 → 新，ms | 配对全程变化 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 1.558 → 1.495 | 22.038 → 3.703 | 23.594 → 5.419 | −77.1% |
| 200 万 | 4.175 → 4.117 | 21.588 → 3.693 | 25.694 → 7.923 | −69.9% |
| 1.28 亿 | 170.401 → 169.615 | 21.821 → 3.606 | 191.912 → 173.775 | −9.5% |

三档的排空耗时在每一对开机中都下降。短任务的第一次和第二次 spawn 分别从 69.70 / 63.70 µs 到 68.40 / 63.95 µs，关闭从 44.50 到 44.45 µs。创建者在此计时负载中保留原始映射，排空表示模板引用和作业都已清理。共享原帧最终入池还取决于创建者自身释放映射。另测了 freeze(1) 的 200 万次工作，全程从 4.59 到 4.76 ms，配对变化 +6.45%，各对从 −0.95% 到 +8.74%。该策略继续走原有代码路径。[spawn、close 与参照策略的各对结果](summary.json)

## 直接测量 wait 和两项改动

单独的 latewait 程序让两个子进程先进入 ZOMBIE，父进程确认其状态后分别给两次 wait 计时。进程状态探针来自 LIFECYCLE=check，两侧使用同一配置，此段计时未打开生命周期收集。四对开机中第一次 wait 从 122.70 到 140.05 µs，配对 +11.8%，各对 −0.8% 至 +28.0%。最后一次 wait 从 114.85 到 117.50 µs，配对 +2.44%，各对 +1.39% 至 +4.70%。这组负载的 wait 后排空从 23.69 到 3.62 ms。主要收益落在后台排空阶段，前台有微秒级代价。[僵尸进程的配对原始数据](zombie-waits.json)

对极短工作量另做四种配置，每种配置开机四次，以平衡顺序交替，每次预热一轮、测三轮。仅在最后一支离开时允许直接按终态释放，尾部由 24.56 到 22.16 ms。仅允许在显式 yield 时给回收器连续的有界调度机会，到 8.67 ms。两项合用为 3.59 ms。四个区组都得到相同排序。旧版的主要代价来自相邻回收批次之间的调度等待，在此基础上跳过结束后的确认与翻译刷新又有独立收益。这是对本工作负载的归因。[四种组合的开机结果](ablation/comparison.json) · [终态单项补丁](ablation/terminal.patch) · [让出单项补丁](ablation/donor.patch)

## 页池与作业重叠

- 四页物理检查在真实关闭入口停住启动 hart 时，另一 hart 仍在分支 U 态读原页。两个分支各保留两页、撤掉另外两页，创建者撤去四页。观察到引用从 [2,2,2,2] 到 [1,0,1,0]，最终全零。空闲链表相应从零到两帧再到四帧。关闭前后均捕获两个实际读者。[QMP 页表和页池](checks/physical.json) · [关闭入口寄存器](checks/at-close.txt)
- 五窗试验在两个活分支各保留五页时有 2038 个原帧回池。两支回收后 2048 个原帧都回池，派生时借用堆帧零增引用。保留创建者映射的另一轮则有 2048 次减引用、零帧当场回池。两个用例均逐批核对 32/8/1 上限。[逐批检查与页池计数](checks/five-window.txt)
- 最后一支执行 exec 后，新程序仍活着时父进程已经观察到旧作业排空。另让两个作业重叠，创建者保留映射，观察到 4096 次减引用、零帧提前入池、两个作业均析构，并有队列轮换。两种情况均核对创建者的原始字节和最终资源余额。[exec、重叠与余额](checks/exec-and-overlap.txt)
- 标准 make run 在 QEMU 双核开关的两态均进入 Shell 并接受 help，入口 0x80200000。VisionFive2 构建 ELF、FIT，入口 0x40200000。本轮计时均来自 QEMU。[默认启动](checks/default-boot.txt) · [双核启动](checks/dual-boot.txt) · [构建与镜像清单](versions.json)

## 复跑

从本研究分支的 Ubuntu 仓库根目录执行以下命令。基线与候选分目录构建并直接引导。调试 ELF 会记录构建路径，有原始产物时可直接代入 versions.json 已记录的 ELF 和磁盘。

~~~sh
out="$PWD/build/last-detach-replay"
mkdir -p "$out/old-src"
git archive a09d365 | tar -x -C "$out/old-src"
cfg=CONFIG=riscv64-qemu-virt-sbi
for mode in time check; do
    make -s -j4 -C "$out/old-src" "$cfg" BUILD="$out/old-$mode" DUAL_HART_USERS=1 LIFECYCLE="$mode" build
    make -s -j4 "$cfg" BUILD="$out/new-$mode" DUAL_HART_USERS=1 LIFECYCLE="$mode" build
done
apps="$out/apps"
make -s -j4 "$cfg" BUILD="$apps" LIFECYCLE=time "$apps/apps/sh.bin" "$apps/apps/tdbench.bin" "$apps/apps/bopen.bin"
python3 tools/mkfs.py "$out/td.img" --program sh "$apps/apps/sh.bin" --program tdbench "$apps/apps/tdbench.bin" --program bopen "$apps/apps/bopen.bin"
python3 tools/bench_last_detach.py "$out/old-time/kernel.elf" "$out/new-time/kernel.elf" "$out/td.img" "$out/primary"
riscv64-linux-gnu-gcc -Iinclude -Iuser -Itools -std=c11 -Os -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -mno-relax -march=rv64imac -mabi=lp64 -nostdlib -static -Wl,--build-id=none,--no-relax,-T,user/program.ld user/start.S user/lib.c kernel/lib/string.c tools/last_detach_wait.c -o "$out/latewait.elf"
riscv64-linux-gnu-objcopy -O binary "$out/latewait.elf" "$out/latewait.bin"
python3 tools/mkfs.py "$out/wait.img" --program sh "$apps/apps/sh.bin" --program latewait "$out/latewait.bin"
python3 tools/bench_last_detach.py "$out/old-check/kernel.elf" "$out/new-check/kernel.elf" "$out/wait.img" "$out/waits" --zombies
~~~

用同一检查镜像运行 latewait exec 和 latewait overlap。四页物理检查复用[上一轮的 held 来宾与宿主读数程序](../dual-hart-template-branches/independent/README.md)，传入新计时 ELF 及 --branch-parallel。四种配置对照可接着执行以下命令。

~~~sh
proof="$PWD/docs/process-lifecycle/template-last-detach-drain/ablation"
for variant in terminal donor; do
    src="$out/src-$variant"
    mkdir -p "$src"
    if [ "$variant" = terminal ]; then ref=130be23; else ref=a09d365; fi
    git archive "$ref" | tar -x -C "$src"
    patch -s -d "$src" -p1 < "$proof/$variant.patch"
    make -s -j4 -C "$src" "$cfg" BUILD="$out/$variant" DUAL_HART_USERS=1 LIFECYCLE=time build
done
python3 "$proof/compare.py" "$out/old-time/kernel.elf" "$out/terminal/kernel.elf" "$out/donor/kernel.elf" "$out/new-time/kernel.elf" "$out/td.img" "$out/ablation"
~~~

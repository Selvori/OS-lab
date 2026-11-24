# 密集系统调用的启动 hart 亲和策略

初版双 hart `2ab40ba3015751b852a835f7a5ae85dcf5a58fea` 对独立纯计算有效。原对话用两子进程各连续调用 16,384 次 `getpid` 做了四组交替配对，关闭原型和初版的完成时间中位数分别为 967.19 和 1228.40 ms。每次远端的系统调用都要经过邮箱交接和启动 hart 处理。密集进入内核的任务反复获得短暂远端执行机会，增加了 IPI、切页表和启动 hart 调度开销。[独立初测原始数据](off-vs-initial.json)

签署迭代 `a21fc253825b2d83a33e49558ecafc820d4a9263` 在启动 hart 处理实际 U 态定时器抢占时比较 PCB 已有的 `syscalls` 累计值。相邻两次采样间累计至少 16 次就暂留本地，下一次低于阈值的本地 U 态定时器采样解除限制。IPI 抢占继续按原方式尽快收取回报，保留计数采样窗口。所有系统调用共用这一规则，原系统调用分派及计数路径没有新增逻辑。普通 yield、sleep、wait、COW 和模板回收继续沿用原路径。

## 配对结果

比较均使用同一 `upath` 二进制、磁盘、QEMU 5.1.0、`-smp 2 -accel tcg,thread=multi` 和 `LIFECYCLE=time`。每项比较各四次独立开机配对，交替启动两边，轮换四个工作负载的顺序，先预热再留一个正式样本。计算任务每个子进程做 85,000,000 次运算并核对独立校验值，系统调用任务核对每次 getpid 值。两类都核对 PID、wait 状态及无剩余子进程。

| 每轮工作负载 | 初版 → 新策略 中位数（ms） | 配对中位变化 | 关闭 → 新策略 中位数（ms） | 配对中位变化 |
| --- | ---: | ---: | ---: | ---: |
| 单子进程计算 | 217.33 → 219.29 | +0.90% | 218.04 → 216.67 | −0.79% |
| 双子进程计算 | 218.50 → 219.74 | +0.58% | 438.00 → 218.69 | −50.35% |
| 单子进程密集系统调用 | 484.21 → 482.09 | +0.77% | 478.99 → 484.91 | +0.56% |
| 双子进程密集系统调用 | 1207.94 → 970.10 | **−19.45%** | 961.47 → 980.83 | +1.78% |

双子进程密集系统调用在四对初版对照中分别下降 18.11%、20.51%、21.56%、18.39%。对关闭模式的四对差异为 +1.97%、+1.59%、+0.09%、+2.05%，本次小样本仍观察到轻微额外代价。双子进程计算的中位配对加速为 2.01×。表内独立比较分别采样，故新策略的中位数随比较组而变。[初版对照](initial-vs-adaptive.json) · [关闭原型对照](off-vs-adaptive.json)

GDB 复验中两颗 hart 同暂停时 `priv=0`，同时处于 `calculate`，`satp` 分别为 `0x8000000000080338` 和 `0x8000000000080331`，QEMU 报告两个不同宿主线程。八轮普通生命周期、`bcheck 9` 和 `bclose 9 idle` 均通过，活分支仍实测先回池 2038 帧，最终 2048 帧全部回池。[寄存器](witness-mode-2-gdb.txt) · [回归串口](smoke-serial.log.gz)

阈值按两次启动 hart 的 U 态时钟取样之间的累计次数判断，采样长度会受调度和睡眠影响。首次样本前仍可能发生短交接，低于阈值后要等到下一次符合条件的调度机会。四个固定负载给出本阶段的结果，阈值敏感性、混合工作负载和其他 QEMU 或硬件平台均未测量。

## 复跑

在仓库根目录执行。下面的用户程序和磁盘命令已复验可重建与实测同一 SHA-256 的文件。

```sh
out=build/dual-hart/path-replay
probe=docs/process-lifecycle/dual-hart-users/syscall-affinity/probe
mkdir -p "$out"
cp "$probe/time_paths.py" "$out/"
riscv64-linux-gnu-gcc -Iinclude -Iuser -Itools -std=c11 -Os -Wall -Wextra -Werror \
  -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -mno-relax \
  -march=rv64imac -mabi=lp64 -nostdlib -static \
  -Wl,--build-id=none,--no-relax,-T,user/program.ld \
  user/start.S user/lib.c kernel/lib/string.c "$probe/upath.c" -o "$out/upath.elf"
riscv64-linux-gnu-objcopy -O binary "$out/upath.elf" "$out/upath.bin"
make CONFIG=riscv64-qemu-virt-sbi BUILD="$out/apps" "$out/apps/apps/sh.bin"
python3 tools/mkfs.py "$out/timing.img" --program sh "$out/apps/apps/sh.bin" \
  --program upath "$out/upath.bin"

make CONFIG=riscv64-qemu-virt-sbi BUILD="$out/off" LIFECYCLE=time build
make CONFIG=riscv64-qemu-virt-sbi BUILD="$out/adaptive" DUAL_HART_USERS=1 LIFECYCLE=time build
git worktree add --detach "$(realpath "$out")/initial-source" 2ab40ba3015751b852a835f7a5ae85dcf5a58fea
make -C "$out/initial-source" CONFIG=riscv64-qemu-virt-sbi \
  BUILD="$(realpath "$out")/initial" DUAL_HART_USERS=1 LIFECYCLE=time build
python3 "$out/time_paths.py" "$out/initial/kernel.elf" "$out/adaptive/kernel.elf"
mv "$out/paths.json" "$out/initial-vs-adaptive.json"
python3 "$out/time_paths.py" "$out/off/kernel.elf" "$out/adaptive/kernel.elf"
mv "$out/paths.json" "$out/off-vs-adaptive.json"

python3 tools/bench_dual_hart.py build/dual-hart/path-witness --witness-only
python3 tools/bench_dual_hart.py build/dual-hart/path-smoke --smoke
```

原始 JSON 的 `solo` 字段对应脚本第一个 ELF，`dual` 对应第二个 ELF。原始数据保留了预热结果、各次启动 hart 及工件摘要。

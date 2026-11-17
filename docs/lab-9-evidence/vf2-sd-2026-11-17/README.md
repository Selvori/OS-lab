# VisionFive 2 microSD 存储验收

板卡为 V1.3B。启动卡 SK64G 由板端 Ubuntu 识别为 `/dev/mmcblk1`，共 124735488 个 512 字节扇区。Linux sysfs 指向 JH7110 `0x16020000` 控制器。GPT 验证通过，原卡尾未分配范围为 LBA 112263168–124735454。

在板端保存 GPT 文本及首尾原始扇区后追加了单独的 `seelefs` 分区。起点 112263168，结束 112271359，长度 8192 扇区，分区类型为 `9ea193f1-66ec-4307-a879-474c515e4e45`，实际 PARTUUID 为 `2bbd7c93-de6a-4af7-8732-1ed5f49d48ff`。追加后重新校验 GPT 并核对原四个条目的起止位置、类型和 UUID。4 MiB 镜像内含 `/bin/sh` 和 `/bin/echo`，通过 PARTUUID 写入新分区，fsync 后完整读回的 SHA-256 为 `83f32b9636cd1ccf8f3f0724f9dca4ac1a2ca060d15db1399e6f55d41c710d24`。

## 内核路径

U-Boot 在 `mmc dev 1` 和从 SD 加载 DTB 时启用控制器时钟和板级引脚。Seele 复用该板级状态，重新初始化卡协议，使用 3.3 V、单线、轮询 PIO。容量来自 CSD v2；单扇区访问分别使用 CMD17/CMD24，写入返回前通过 CMD13 确认就绪。用户态不能映射 SDIO1 MMIO。

`drivers/block/gpt.c` 核对主、备 GPT 的头和表 CRC，检查条目范围、重叠及测试类型是否唯一。块缓存看到的是分区内扇区号；数据读写只经过分区范围校验后的映射。这条规则允许其他启动分区保留原格式。QEMU 构建仍使用 VirtIO 后端。板卡与寄存器参考 [VF2 设备树](https://github.com/torvalds/linux/blob/master/arch/riscv/boot/dts/starfive/jh7110-starfive-visionfive-2.dtsi)、[Linux DW-MMC 寄存器定义](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/dw_mmc.h) 和 [U-Boot DW-MMC 主机驱动](https://github.com/u-boot/u-boot/blob/master/drivers/mmc/dw_mmc.c)。

## 验收命令

~~~bash
make CONFIG=riscv64-visionfive2-uboot build image
python3 tools/test_vf2_gpt.py
python3 tools/test_lab9.py
~~~

主机 GPT 测试覆盖正常分区、测试分区缺失、错误类型、错误长度、重复或重叠、超界、主备 CRC 损坏。QEMU Lab 9 的 Shell、描述符、exec 和旧镜像回退回归通过。板端使用 Windows COM7 将 FIT 载入 RAM，CRC32 `4d8598cc` 与主机一致；U-Boot 的 `bootcmd=run vf2_sdboot` 继续用于正常启动。

首次实机识别控制器版本 `0x290a`，挂载测试分区并从卡加载 Shell。`cat /etc/motd` 与磁盘中的 `/bin/echo` 通过；使用 `write`、`append` 写入 1000 字节文件，跨越 512 字节扇区边界。`sync` 后首次读回逐字节一致。

同一镜像的复位后持久化验收通过。用户按 RST 后，板卡按原配置进入 SD 上的 Ubuntu，随后通过串口正常重启并截停 U-Boot。重新加载同一 FIT 后，SeeleFS 再次挂载并进入 Shell。只读打开 /vf2-check/data，1000 字节与首轮数据及主机期望逐字节一致，SHA-256 为 449b26d3a6438e28382c17f0612b9b04fbf941386ec2e4aa220744fd0f4c6d6f。板端最后运行 RAM 中的 Seele Shell。

块层额外回归沿用 Lab 7 的 LRU、索引回绕和 128 个扇区读写测试，把计数检查改为增量以容纳文件系统启动期的缓存访问。实测通过，包括 QEMU 重启后的扇区回读。

[原 GPT](gpt-before.sfdisk) · [板端分区操作](provision.txt) · [首次启动](first-boot.txt) · [首次读写](first-shell.txt) · [GPT 主机测试](gpt-test.txt) · [QEMU Lab 7 回归](qemu-block-regression.txt) · [QEMU Lab 9 回归](qemu-regression.txt)

[复位后启动](restart-boot.txt) · [复位后读回](restart-shell.txt) · [持久化验收结果](persistence.json)

# Lab 9 VisionFive 2 实机验收

2026-11-17，使用 VisionFive 2 V1.3B 验收 Lab 9 正式内核提交 `1b80b84550cb331904a936583f8d0baf7b4fd040`；在 Ubuntu 虚拟机内执行 `make -B CONFIG=riscv64-visionfive2-uboot build image`。正式 FIT 为 38379 字节，含 36908 字节的内核，入口 `0x40200000`。

VMware 内的 CH340 枚举正常，读取 USB 控制请求时出现 `-110`，重新打开串口出现 EIO。设备交还 Windows 后，CH340 通过 COM7（115200 8N1、无流控）继续验收。板端 Ubuntu 正常重启，截停 U-Boot 2021.10。用 S-record 将 FIT 传到 RAM `0x50000000`，CRC32 `1e255f8a` 与主机一致；从原 SD 第三分区读取 V1.3B DTB 至 `0x51000000`，执行 `bootm 0x50000000 - 0x51000000`。启动配置为 `bootcmd=run vf2_sdboot`，本轮 FIT 仅从 RAM 启动。

| 验收项 | 实测结果 |
| --- | --- |
| 四核启动与 Sv39 | CPU 0–3 均启用同一根页表 `0x000000004021f000` |
| 时钟与 UART | 四核报告 100 Hz SBI 时钟以及 UART 外部中断路由就绪 |
| 用户态回归 | 256 轮堆测试、4096 次系统调用往返、fork/wait、睡眠唤醒、计算任务抢占、32 轮 fork/wait 回收均通过 |
| 真实 UART 回显 | 1153 字节输入含二进制字节与 CR/LF，1155 字节回显逐字节匹配，空闲一秒后仍可通信 |
| 磁盘与 Shell | 板端记录 `block: 0 MiB available`、`SeeleFS: unavailable (-19)`，进入内嵌 Lab 6 程序。物理磁盘、SeeleFS 挂载和磁盘加载的 `/bin/sh` 仍需 VF2 存储后端 |

正式内核运行至 `LAB6 PASS: init pid 0 remains alive`，板端继续运行该 RAM 镜像。时钟动态证据来自用户态睡眠与抢占，四核分别持续计时未在本轮单独采样。

[传输与 CRC 证据](transfer.txt) · [完整启动日志](boot.txt) · [验收结果](result.json)

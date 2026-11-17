# Lab 7 实验报告

## 目标

课程概览给出磁盘与缓冲主题。项目目录和本机课程实验目录只有 Lab 0–6 正式材料，课程是否指定驱动、缓冲算法或开发板验收未核实。本实验按现有 QEMU 5.1.0 virt 平台实现现代 VirtIO 块传输和缓冲接口，供 Lab 8 使用。

## 架构

- `drivers/block/virtio.c` 扫描八个 MMIO 槽，检查魔数、版本和设备标识，建立 split virtqueue，协商版本与 flush。QEMU 5.1.0 默认使用旧传输，因此运行参数显式选择现代传输。
- 读写请求含请求头、512 字节 DMA bounce buffer 和设备状态。flush 只需请求头与状态。描述符、环及数据用内核恒等映射物理地址，I/O fence 约束发布、通知和完成的顺序。
- 驱动轮询 used index，检查完成 ID、长度及状态。超时或遇到无法归属的完成项时停机，以免重用设备可能仍在访问的 DMA 区。
- `kernel/fs/block.c` 在同一自旋锁下查缓存、更新 LRU、串行传输。读缺失填充；写成功更新缓存，失败作废对应项。flush 通过设备发出。
- MMIO 映射仅在内核页表。VF2 返回容量零，普通内核功能仍可启动。当前实现覆盖 QEMU 一致性 DMA、单队列串行轮询和固定容量介质。

传输依据 [OASIS VirtIO 1.1](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html) §§2.6、4.2、5.2，映射核对本地 QEMU 5.1.0 `hw/riscv/virt.c`。

## 过程与验证

在 `b0a225a` 上通过 Lab 6 正式与扩展回归后创建 Lab 7。签署规格提交，完成块层、两平台构建和 QEMU 枚举。独立探针镜像验证 65540 次轮转读、两核并发共 128 个扇区写入、显式 flush 及第二轮启动读回。主机比对最后 128 个扇区的确定性模式和前 8064 个零扇区。逐字节恢复 `main.c` 并重建，正式版本再次启动，验证原 UART 与进程功能。证据见 [验证结果](lab-7-evidence/results.json)。

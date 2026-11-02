# Lab 5 验收证据

2026-11-02的实际构建与QEMU验收。results.json为扩展测试结果，restored-sha256.json为临时修改文件恢复前后的核对值。

serial.txt是串口文本摘录，保留到进程回收提示；原始逐字节UART日志保存在Ubuntu的build/lab5-checks，5777字节精确比较结果见JSON。日志仅统一换行并去除行尾空白。

heap-probe记录1024步位图模型、4096页整堆、8个失败点、128节点和32生命周期。heap-after-free-load/store与heap-nx分别记录预期用户故障；owner-uart和allocation记录继承回归。构建日志、ELF头和FIT识别一并保留。QEMU原生截图位于../../pictures/lab5-user-heap.png，属于另一轮正式运行，计数允许变化。

VisionFive2仅构建验证，本阶段没有实际上板。

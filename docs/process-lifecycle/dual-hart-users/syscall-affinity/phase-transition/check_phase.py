"""Sample both hart privilege levels after each child enters its arithmetic phase."""
import argparse
from pathlib import Path
import socket
import sys
import time

root = Path.cwd()
sys.path.insert(0, str(root / "tools"))
import bench_dual_hart as bench
import mkfs

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path, help="new directory for the guest and logs")
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
here = Path(__file__).resolve().parent
probe = here.parent / "probe"
guest = out / "uphase.elf"
binary = out / "uphase.bin"
disk = out / "phase.img"
kernel = out / "kernel"
shell = kernel / "apps/sh.bin"
flags = bench.make(kernel, True, "time", "build", str(shell))
bench.command("riscv64-linux-gnu-gcc", "-Iinclude", "-Iuser", "-I" + str(probe),
              "-DEXTENDED_OBSERVATION", "-std=c11", "-Os", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
              "-fno-builtin", "-fno-pie", "-fno-stack-protector", "-mno-relax",
              "-march=rv64imac", "-mabi=lp64", "-nostdlib", "-static",
              "-Wl,--build-id=none,--no-relax,-T,user/program.ld", "user/start.S",
              "user/lib.c", "kernel/lib/string.c", str(here / "uphase.c"), "-o", str(guest))
bench.command("riscv64-linux-gnu-objcopy", "-O", "binary", str(guest), str(binary))
mkfs.make_image(disk, programs=[("sh", shell), ("uphase", binary)])
symbols = bench.command("riscv64-linux-gnu-nm", "-S", str(guest)).splitlines()
address, size, _, _ = next(x.split() for x in symbols if x.split()[-1:] == ["crunch"])
start = int(address, 16)
with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]
machine = bench.boot(out, "after-syscalls", flags, disk, port)
try:
    bench.boot_owner(machine)
    machine.send(b"uphase\r")
    for _ in range(2):
        machine.until(b"PHASE COMPUTE\r\n")
    time.sleep(.09)
    rows = bench.inspect(out, "phase-compute", port, kernel / "kernel.elf",
                         (start, start + int(size, 16)), True)
    machine.until(b"USER-PAIR VERIFIED\r\n")
    machine.until(b"seele$ ")
    print(rows)
finally:
    machine.close()

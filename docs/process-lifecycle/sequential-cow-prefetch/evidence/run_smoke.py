"""Build and run the short private-heap COW regression in an isolated build."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "tools"))
import mkfs
import test_lab4 as qemu

out = Path(sys.argv[1]).resolve()
build = Path("build/lifecycle-cow-prefetch-check")
app = out / "cowcheck.bin"
kernel = ["make", "-j4", qemu.CONFIG, f"BUILD={build}", "LIFECYCLE=check"]
with (out / "smoke-build.log").open("wb") as log:
    subprocess.run(kernel + ["build", str(build / "apps/sh.bin")],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(["riscv64-linux-gnu-gcc", "-Iinclude", "-Iuser", "-std=c11", "-Os",
                    "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin",
                    "-fno-pie", "-fno-stack-protector", "-mno-relax", "-march=rv64imac",
                    "-mabi=lp64", "-nostdlib", "-static",
                    "-Wl,--build-id=none,--no-relax,-T,user/program.ld",
                    "user/start.S", "user/lib.c", "kernel/lib/string.c",
                    str(Path(__file__).with_name("smoke.c")), "-o", str(app.with_suffix(".elf"))],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(["riscv64-linux-gnu-objcopy", "-O", "binary", str(app.with_suffix(".elf")), str(app)],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
image = out / "smoke.img"
mkfs.make_image(image, programs=[("sh", build / "apps/sh.bin"), ("cowcheck", app)])
os.environ["MAKEFLAGS"] = f"BUILD={build} LIFECYCLE=check"
os.environ["DISK"] = str(image)
qemu.OUT = out
machine = qemu.Machine("prefetch-smoke")
try:
    machine.until(b"seele$ ")
    for _ in range(4):
        machine.send(b"cowcheck\r")
        result = machine.until(b"seele$ ")
        assert b"PREFETCH PASS" in result and result.count(b"PREFETCH CASE") == 5, result
        assert all(marker not in result for marker in (b"FAIL", b"PANIC", b"USER FAULT")), result
finally:
    machine.close()

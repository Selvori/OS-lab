import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path.cwd() / "tools"))
import mkfs
import test_lab4 as qemu

out = Path(sys.argv[1]).resolve()
qemu.OUT = out
qemu_conf = "riscv64-qemu-virt-sbi"
board_conf = "riscv64-visionfive2-uboot"
app = Path("build") / qemu_conf / "apps" / "sh.bin"
with (out / "formal-build.log").open("wb") as log:
    for config, targets in ((qemu_conf, ["build", str(app)]), (board_conf, ["build", "image"])):
        subprocess.run(["make", "-j4", "CONFIG=" + config, *targets],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
with (out / "formal-entry.txt").open("w") as report:
    for config, want in ((qemu_conf, "0x80200000"), (board_conf, "0x40200000")):
        elf = str(Path("build") / config / "kernel.elf")
        header = subprocess.check_output(["riscv64-linux-gnu-readelf", "-h", elf], text=True)
        entry = next(line.strip() for line in header.splitlines() if "Entry point" in line)
        assert entry.endswith(want), (elf, entry)
        nm = subprocess.check_output(["riscv64-linux-gnu-nm", elf], text=True)
        assert "heap_ref_trace" not in nm and "lifecycle_control" not in nm
        print(config, entry, file=report)
fit = "build/riscv64-visionfive2-uboot/kernel.itb"
listing = subprocess.check_output(["dumpimage", "-l", fit])
assert b"0x40200000" in listing
(out / "formal-fit.txt").write_bytes(listing)
with tempfile.TemporaryDirectory(dir=out) as tmp:
    disk = Path(tmp) / "formal.img"
    mkfs.make_image(disk, programs=[("sh", app)])
    os.environ["DISK"] = str(disk)
    machine = qemu.Machine("formal")
    try:
        initial = machine.until(b"seele$ ")
        assert b"init: starting /bin/sh" in initial
        machine.send(b"pwd\r")
        assert machine.until(b"seele$ ") == b"pwd\r\n/\r\nseele$ "
        assert b"PANIC" not in machine.log and b"USER FAULT" not in machine.log
    finally:
        machine.close()

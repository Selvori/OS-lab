from pathlib import Path
import hashlib
import os

import test_lab4 as qemu

qemu.OUT = Path(os.environ["OUTDIR"]).resolve()
path = qemu.ROOT / "kernel/main.c"
original = path.read_bytes()
src = original.decode("utf-8")
assert src.count("void main(void)") == src.count("        page_allocator_init();") == 1
probe = (qemu.OUT / "smoke.c.txt").read_text(encoding="utf-8")
try:
    path.write_text(src.replace("#include <kernel/trap.h>",
                                "#include <kernel/trap.h>\n#include <kernel/string.h>")
                    .replace("void main(void)", probe + "void main(void)")
                    .replace("        page_allocator_init();",
                             "        page_allocator_init();\n        rv64_page_smoke();"),
                    encoding="utf-8")
    os.environ["MAKEFLAGS"] = "BUILD=build/rv64-page-copy-smoke"
    qemu.build("smoke")
    machine = qemu.Machine("smoke")
    try:
        machine.until(b"RV64 PAGE SMOKE PASS: 144 copies, 180 fills, 2 page pools\r\n")
        machine.until(b"seele$ ")
        assert b"PANIC" not in machine.log and b"USER FAULT" not in machine.log
    finally:
        machine.close()
finally:
    path.write_bytes(original)
    assert path.read_bytes() == original
    (qemu.OUT / "restored-main.sha256").write_text(
        hashlib.sha256(original).hexdigest() + "  kernel/main.c\n")

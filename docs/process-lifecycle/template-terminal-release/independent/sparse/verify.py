"""Inspect the same 26 anonymous frames in a normal, uninstrumented Seele guest."""
from collections import Counter
import argparse
import gzip
import json
import mmap
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "docs/process-lifecycle/template-bounded-reclaim/independent"))
import physical as ph

SPANS = [(0, 3, True), (58, 10, False), (126, 2, True), (508, 6, False),
         (1020, 4, True), (4095, 1, False)]
KEPT = {i for start, count, keep in SPANS if keep for i in range(start, start + count)}
OFFSETS = [i for start, count, _ in SPANS for i in range(start, start + count)]
HOLES = (3, 57, 68, 125, 128, 507, 514, 1019, 1024, 4094)
PHASES = ("ORIGIN", "BEFORE CLOSE", "DRAINED", "FINAL")


def verify(phase, mem, sym, pid, original):
    ram = ph.RAM(mem, sym)
    root = ram.process(pid)
    assert root is not None
    assert all(ram.mapping(root, ph.HEAP + i * ph.PAGE) is None for i in HOLES)
    mapped = {i: ram.mapping(root, ph.HEAP + i * ph.PAGE) for i in OFFSETS}
    if phase == "ORIGIN":
        original.update(mapped)
        assert None not in original.values() and len(set(original.values())) == len(OFFSETS)
    free = []
    histogram = Counter()
    for i, frame in original.items():
        parent_holds = phase == "ORIGIN" or (phase != "FINAL" and i in KEPT)
        refs = int(parent_holds) + int(phase == "BEFORE CLOSE")
        assert mapped[i] == (frame if parent_holds else None), (phase, i, mapped[i])
        assert ram.refs(frame) == refs, (phase, i, refs, ram.refs(frame))
        assert (frame in ram.free) == (refs == 0), (phase, i, refs)
        if parent_holds:
            assert ram.number(frame, "<B") == 0x40 + (i * 17 % 61)
        if frame in ram.free:
            free.append(i)
        histogram[refs] += 1
    return dict(phase=phase, refs=dict(histogram), on_free_list=len(free), free_offsets=free)


def run(revision, policy, out):
    out.mkdir(parents=True, exist_ok=True)
    build = out / "normal"
    apps = build / "apps"
    rule = "$(BUILD)/apps/sparseaudit.elf: $(AUDIT_SOURCE) $(APP_DEPS)\n\t$(compile_user_program)\n"
    src = Path(__file__).resolve().with_name("sparseaudit.c")
    subprocess.run(["make", "-s", "-j2", "-f", "Makefile", "-f", "-", "CONFIG=riscv64-qemu-virt-sbi",
                    f"BUILD={build}", f"AUDIT_SOURCE={src}", "build",
                    str(apps / "sh.bin"), str(apps / "sparseaudit.bin")],
                   input=rule, text=True, check=True)
    elf = build / "kernel.elf"
    sym = ph.symbols(elf)
    info = dict(revision=revision, policy=policy, elf=ph.filehash(elf),
                guest=ph.filehash(apps / "sparseaudit.bin"), snapshots=[])
    original = {}
    with tempfile.TemporaryDirectory(prefix="seele-terminal-audit-") as temp:
        work = Path(temp)
        disk, qmp, dump = work / "disk.img", work / "mon.sock", work / "ram.bin"
        ph.mkfs.make_image(disk, programs=[("sh", apps / "sh.bin"), ("audit", apps / "sparseaudit.bin")])
        cmd = ["qemu-system-riscv64", "-machine", "virt", "-bios", "default", "-m", "128M",
               "-smp", "2", "-display", "none", "-serial", "stdio", "-monitor", "none",
               "-qmp", f"unix:{qmp},server,nowait", "-global", "virtio-mmio.force-legacy=false",
               "-drive", f"file={disk},if=none,format=raw,id=seele-disk",
               "-device", "virtio-blk-device,drive=seele-disk", "-kernel", str(elf)]
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        serial = ph.Serial(proc)
        try:
            serial.until(b"seele$ ")
            with socket.socket(socket.AF_UNIX) as mon:
                mon.settimeout(20)
                mon.connect(str(qmp))
                with mon.makefile("rwb") as wire:
                    assert "QMP" in json.loads(wire.readline())
                    ph.send("qmp_capabilities", wire)
                    serial.write(f"audit {policy}\r".encode())
                    for phase in PHASES:
                        serial.until(f"AUDIT {phase}\r\n".encode())
                        pid = int(re.search(rb"AUDIT PID (\d+)", serial.log)[1])
                        ph.send("stop", wire)
                        assert ph.send("query-status", wire)["running"] is False
                        ph.send("pmemsave", wire, val=ph.DRAM, size=ph.DRAM_BYTES, filename=str(dump))
                        with dump.open("rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mem:
                            info["snapshots"].append(verify(phase, mem, sym, pid, original))
                        dump.unlink()
                        ph.send("cont", wire)
                        serial.write(b"g")
                    serial.until(b"TERMINAL AUDIT PASS\r\n")
                    serial.until(b"seele$ ")
                    ph.send("quit", wire)
            proc.wait(timeout=5)
            info["passed"] = True
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=5)
            (out / "serial.log.gz").write_bytes(gzip.compress(bytes(serial.log), mtime=0))
            (out / "physical.json").write_text(json.dumps(info, indent=2) + "\n")
    print([(s["phase"], s["on_free_list"], s["refs"]) for s in info["snapshots"]])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    run(revision, args.policy, args.output.resolve())

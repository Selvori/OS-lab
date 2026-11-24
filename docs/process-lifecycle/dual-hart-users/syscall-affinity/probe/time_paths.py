"""Alternate on/off QEMU boots; take one warm-up then one recorded round per case."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys

root = Path.cwd()
sys.path.insert(0, str(root / "tools"))
from test_lab4 import Machine

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("solo", type=Path)
parser.add_argument("dual", type=Path)
parser.add_argument("--pairs", type=int, default=4)
args = parser.parse_args()
work = Path(__file__).resolve().parent
disk = work / "timing.img"
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def boot_kernel(elf, case_order):
    machine = Machine.__new__(Machine)
    machine.pending = bytearray()
    machine.log = bytearray()
    command = ["qemu-system-riscv64", "-machine", "virt", "-accel", "tcg,thread=multi",
               "-bios", "default", "-m", "128M", "-smp", "2", "-display", "none",
               "-monitor", "none", "-serial", "stdio",
               "-global", "virtio-mmio.force-legacy=false",
               "-drive", f"file={disk},if=none,format=raw,snapshot=on,id=d",
               "-device", "virtio-blk-device,drive=d", "-kernel", str(elf)]
    machine.p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, bufsize=0)
    try:
        machine.until(b"seele$ ")
        rows = []
        for case in case_order * 2:
            machine.send(f"upath {case}\r".encode())
            out = machine.until(b"seele$ ", timeout=30)
            match = re.search(rb"PATH OK (\d+) (\d+) (\d+)\r?\n", out)
            if not match:
                raise RuntimeError(out)
            children, count, ticks = map(int, match.groups())
            assert children == int(case[-1]) and count == (85000000 if case.startswith("cpu") else 16384)
            rows.append(ticks)
        owner = re.search(r"owner cpu=(\d+)", machine.log.decode()).group(1)
        return dict(owner=int(owner), guest_ticks=dict(zip(case_order, rows[len(case_order):])),
                    warmup=dict(zip(case_order, rows[:len(case_order)])))
    finally:
        machine.p.terminate()
        machine.p.wait(timeout=5)

cases = ["cpu 1", "cpu 2", "sys 1", "sys 2"]
runs = []
for i in range(args.pairs):
    order = ["solo", "dual"] if i % 2 == 0 else ["dual", "solo"]
    pair = {}
    case_order = cases[i % len(cases):] + cases[:i % len(cases)]
    for variant in order:
        pair[variant] = boot_kernel(getattr(args, variant), case_order)
    runs.append(pair)

med = statistics.median
summary = {}
for c in cases:
    solo = [p["solo"]["guest_ticks"][c] for p in runs]
    dual = [p["dual"]["guest_ticks"][c] for p in runs]
    summary[c] = dict(solo_ms=med(solo) / 10000, dual_ms=med(dual) / 10000,
                      paired_ratio=med(d / s for d,s in zip(dual,solo)))
report = dict(sha256={n:sha(getattr(args,n)) for n in ("solo","dual")},
              disk_sha256=sha(disk), user_sha256=sha(work / "upath.bin"),
              timebase_hz=10000000, pairs=runs, median=summary)
(work / "paths.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(summary))

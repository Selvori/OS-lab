#!/usr/bin/env python3
"""Compare all seven running policies using the archived bopen benchmark."""
import argparse
from collections import Counter
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "tools"))
from bench_bounded_reclaim import sample
import mkfs
import test_lab4 as qemu

CPUS = 2
POLICIES = range(7)
BASE_ORDER = (0, 1, 6, 2, 5, 3, 4)
REPORT = ROOT / "docs/process-lifecycle/template-bounded-reclaim"

def orders():
    result = []
    for shift in POLICIES:
        forward = [(x + shift) % 7 for x in BASE_ORDER]
        result.extend([forward, forward[::-1]])
    for position in POLICIES:
        assert sorted(row[position] for row in result) == sorted(list(POLICIES) * 2)
    adjacent = Counter((a, b) for row in result for a, b in zip(row, row[1:]))
    assert len(adjacent) == 42 and set(adjacent.values()) == {2}
    return result

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def archive(path, data):
    path.write_bytes(gzip.compress(data, compresslevel=6, mtime=0))

class Guest(qemu.Machine):
    def __init__(self, label, kernel, disk):
        self.label = label
        self.log = bytearray()
        self.pending = bytearray()
        cmd = ["qemu-system-riscv64", "-machine", "virt", "-bios", "default",
               "-m", "128M", "-smp", str(CPUS), "-nographic",
               "-global", "virtio-mmio.force-legacy=false",
               "-drive", f"file={disk},if=none,format=raw,id=seele-disk",
               "-device", "virtio-blk-device,drive=seele-disk", "-kernel", str(kernel)]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, bufsize=0, start_new_session=True)

def measure(model, i, order, kernel, apps, out):
    label = f"{model}-{i:02d}"
    result = dict(boot=i, order=order, warm=[], samples=[])
    with tempfile.TemporaryDirectory(dir=out) as work:
        disk = Path(work) / "guest.img"
        mkfs.make_image(disk, programs=list(apps.items()))
        vm = Guest(label, kernel, disk)
        try:
            vm.until(b"seele$ ")
            for n, key in ((2, "warm"), (64, "samples")):
                for policy in order:
                    case = sample(vm, f"bopen {policy} {n} 512", "OPEN PASS")
                    rows = case["rows"]
                    assert len(rows["OPEN"]) == 1 and rows["OPEN"][0][:3] == [policy,n,512]
                    assert len(rows["ROUND"]) == n
                    result[key].append(case)
        finally:
            vm.close()
        result["serial_sha256"] = hashlib.sha256(vm.log).hexdigest()
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", type=Path, help="directory with build-time and build-counts")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    qemu.OUT = out
    order = orders()
    result = dict(checkout=subprocess.check_output(["git","rev-parse","HEAD"],text=True).strip(),
                  qemu=subprocess.check_output(["qemu-system-riscv64","--version"],text=True).splitlines()[0],
                  harts=CPUS, guest_memory_MiB=128, timebase_hz=10_000_000, modes={})
    log_parts = []
    for mode in ("time", "counts"):
        with gzip.open(REPORT / "raw" / f"{mode}.json.gz","rt",encoding="utf-8") as f:
            ref = json.load(f)
        build = args.artifacts.resolve() / ("build-" + mode)
        apps = {name:build/"apps"/(name+".bin") for name in ("sh","bopen")}
        kernel = build/"kernel.elf"
        hashes = {k:digest(v) for k,v in dict(apps,kernel=kernel).items()}
        assert all(hashes[k] == ref["hashes"][k] for k in apps), mode
        group = dict(hashes=hashes, original_kernel_sha256=ref["hashes"]["kernel"],
                     same_elf=(hashes["kernel"]==ref["hashes"]["kernel"]),
                     original_measurement_revision=ref["commit"], boots=[])
        result["modes"][mode] = group
        for i, row in enumerate(order if mode == "time" else order[:1]):
            group["boots"].append(measure(mode,i,row,kernel,apps,out))
            log = out/f"{mode}-{i:02d}-serial.log"
            log_parts.extend([f"\n--- {log.name} ---\n".encode(), log.read_bytes()])
        print(mode, "boots", len(group["boots"]), "same archived ELF", group["same_elf"], flush=True)
    archive(out/"raw.json.gz", (json.dumps(result,separators=(",",":"))+"\n").encode())
    archive(out/"serial.log.gz", b"".join(log_parts))

if __name__ == "__main__":
    main()

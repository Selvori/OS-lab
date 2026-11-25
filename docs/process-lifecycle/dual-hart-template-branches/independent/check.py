"""Check live guest page tables, physical free list and hart registers for four original frames."""
import argparse
import json
import mmap
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

root = Path.cwd()
sys.path.insert(0, str(root / "docs/process-lifecycle/template-bounded-reclaim/independent"))
import physical as ph

args = argparse.ArgumentParser(description=__doc__)
args.add_argument("kernel", type=Path)
args.add_argument("assets", type=Path, help="directory with disk.img and held.elf")
args.add_argument("output", type=Path)
args.add_argument("policy", type=int, choices=(1, 9))
args.add_argument("--branch-parallel", action="store_true")
opt = args.parse_args()
opt.output.mkdir(parents=True, exist_ok=True)
address = (0x3000, 0x4000, 0x200000, 0x400000)
symbols = ph.symbols(opt.kernel)
nm = subprocess.check_output(["riscv64-linux-gnu-nm", "-S", str(opt.assets / "held.elf")], text=True)
code, length = (int(n, 16) for n in re.search(r"^(\w+) (\w+) t read_held$", nm, re.M).groups())
satp_mode = 8 << 60
reg_command = r'thread apply all printf "HREG %lu %lu %lx %lx\n", $mhartid, $priv, $pc, $satp'
def regs(output):
    found = re.findall(r"HREG (\d+) (\d+) ([0-9a-f]+) ([0-9a-f]+)", output)
    entries = sorted([dict(hart=int(hart), priv=int(priv), pc=int(pc, 16), satp=int(satp, 16))
                      for hart, priv, pc, satp in found], key=lambda r:r["hart"])
    assert len(entries) == 2 and entries[0]["hart"] != entries[1]["hart"], output
    return entries

def debug(*commands):
    argv = ["gdb-multiarch", "-nx", "-batch", str(opt.kernel), "-ex", f"target remote 127.0.0.1:{port}"]
    for command in commands:
        argv.extend(["-ex", command])
    return argv

def page_table(ram, root_pa, va):
    middle=(ram.u64(root_pa + 8*((va >> 30) & 511)) >> 10) << 12
    return (ram.u64(middle + 8*((va >> 21) & 511)) >> 10) << 12

def snapshot(stage, original, parent_pid, children, wire, scratch):
    # The checkpoint lasts until the host sends a byte. Retry a CPU sample if
    # an interrupt or the creator's console wait interrupted a reader.
    for attempt in range(14):
        time.sleep(.034 if attempt == 0 else .019 + attempt * .004)
        ph.send("stop", wire)
        assert not ph.send("query-status", wire)["running"]
        dump = scratch / "ram.bin"
        ph.send("pmemsave", wire, val=ph.DRAM, size=ph.DRAM_BYTES, filename=str(dump))
        with dump.open("rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mem:
            ram = ph.RAM(mem, symbols)
            parent = ram.process(parent_pid)
            assert parent
            roots = [ram.process(pid) for pid in children]
            creator_map = [ram.mapping(parent, va) for va in address]
            if stage == "open":
                original = creator_map
                assert all(original) and len(set(original)) == 4
            else:
                assert creator_map == [None] * 4, (stage, creator_map)
            if stage == "final":
                assert roots == [None, None]
                mappings, leaf_tables = [], []
                expected = [0] * 4
            else:
                assert all(roots) and len(set(roots)) == 2
                mappings = [[ram.mapping(r, va) for va in address] for r in roots]
                assert all(row == [original[0], None, original[2], None] for row in mappings), mappings
                leaf_tables = [[page_table(ram, r, address[i]) for i in (0,2)] for r in roots]
                assert leaf_tables[0][0] != leaf_tables[1][0] and leaf_tables[0][1] == leaf_tables[1][1]
                expected = [2,2,2,2] if stage == "open" else ([1,1,1,1] if opt.policy == 1 else [1,0,1,0])
            references = [ram.refs(frame) for frame in original]
            free = [frame in ram.free for frame in original]
            assert references == expected and free == [r == 0 for r in expected], (stage, references, free, expected)
        dump.unlink()
        if stage == "final":
            ph.send("cont", wire)
            harts = []
            readers = []
        else:
            # The RAM snapshot was taken with the VM paused. GDB attaches to
            # that stopped VM, reads both CPUs, then resumes it on detach.
            log = subprocess.check_output(debug(reg_command, "detach"), stderr=subprocess.STDOUT,
                                          timeout=8, text=True)
            harts = regs(log)
            readers = [r for r in harts if r["priv"] == 0 and code <= r["pc"] < code + length
                       and r["satp"] in [satp_mode | (p >> 12) for p in roots]]
            if not ph.send("query-status", wire)["running"]:
                ph.send("cont", wire)
            if len(readers) != (2 if opt.branch_parallel else 1):
                continue
            assert len({r["satp"] for r in readers}) == len(readers)
        row = dict(stage=stage, original=original, refs=references, on_free_list=free,
                   roots=roots, mappings=mappings, leaf_tables=leaf_tables,
                   readers=len(readers), harts=harts, attempts=attempt+1)
        return original, row
    raise AssertionError(f"No stopped sample captured the expected number of U-mode readers in phase {stage}")

with socket.socket() as free_port:
    free_port.bind(("127.0.0.1", 0))
    port = free_port.getsockname()[1]
disk=opt.assets / "disk.img"
report = dict(policy=opt.policy, kernel_sha256=ph.filehash(opt.kernel), disk_sha256=ph.filehash(disk),
              guest_sha256=ph.filehash(opt.assets / "held.bin"), virtual_addresses=address, phases=[])
with tempfile.TemporaryDirectory(prefix="seele-held-") as tmp:
    scratch=Path(tmp)
    monitor=scratch/"qmp.sock"
    cmd=["qemu-system-riscv64", "-machine", "virt", "-accel", "tcg,thread=multi", "-bios", "default",
         "-m", "128M", "-smp", "2", "-display", "none", "-monitor", "none", "-serial", "stdio",
         "-gdb", f"tcp:127.0.0.1:{port}", "-qmp", f"unix:{monitor},server,nowait",
         "-global", "virtio-mmio.force-legacy=false", "-drive", f"file={disk},if=none,format=raw,snapshot=on,id=d",
         "-device", "virtio-blk-device,drive=d", "-kernel", str(opt.kernel)]
    guest=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    serial=ph.Serial(guest)
    try:
        serial.until(b"seele$ ", timeout=10)
        serial.write(f"held {opt.policy}\r".encode())
        serial.until(b"HELD OPEN\r\n", timeout=12)
        family=list(map(int,re.search(rb"FAMILY (\d+) (\d+) (\d+)", serial.log).groups()))
        parent, children=family[0],family[1:]
        report["family"]=family
        with socket.socket(socket.AF_UNIX) as qmp:
            qmp.settimeout(15)
            qmp.connect(str(monitor))
            with qmp.makefile("rwb") as wire:
                assert "QMP" in json.loads(wire.readline())
                ph.send("qmp_capabilities", wire)
                original, row=snapshot("open", None, parent, children, wire, scratch)
                report["phases"].append(row)
                armed=scratch/"armed"
                close_args=debug("hbreak template_close",
                    f'python open({str(armed)!r}, "w").close()', "continue",
                    r'printf "CLOSE_OWNER %d %d %lx\n", current[scheduler_owner]->pid, remote_proc ? remote_proc->pid : -1, remote_proc ? (unsigned long)remote_proc->pgtbl : 0',
                    reg_command, "detach")
                with subprocess.Popen(close_args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True) as inspector:
                    try:
                        end=time.monotonic()+6
                        while not armed.exists() and inspector.poll() is None and time.monotonic()<end:
                            time.sleep(.01)
                        assert armed.exists(), "close breakpoint was not installed"
                        serial.write(b"g")
                        output,_=inspector.communicate(timeout=8)
                    finally:
                        if inspector.poll() is None:
                            inspector.kill()
                (opt.output/"at-close.txt").write_text(output)
                owner, remote, page_root = re.search(r"CLOSE_OWNER (\d+) (-?\d+) ([0-9a-f]+)",output).groups()
                assert int(owner) == parent
                harts=regs(output)
                executing = any(r["priv"] == 0 and code <= r["pc"] < code+length
                                and r["satp"] == (satp_mode | (int(page_root,16)>>12)) for r in harts)
                report["at_close"]=dict(creator=int(owner), assigned_pid=int(remote),
                                        remote_running_reader=executing, harts=harts)
                serial.until(b"HELD CLOSED\r\n", timeout=6)
                _, row=snapshot("closed", original, parent, children, wire, scratch)
                report["phases"].append(row)
                serial.write(b"g")
                serial.until(b"HELD FINAL\r\n", timeout=8)
                _, row=snapshot("final", original, parent, children, wire, scratch)
                report["phases"].append(row)
                serial.write(b"g")
                serial.until(b"HELD PASS\r\n", timeout=5)
                serial.until(b"seele$ ")
                ph.send("quit",wire)
        report["passed"]=True
    finally:
        if guest.poll() is None: guest.terminate()
        guest.wait(timeout=4)
        (opt.output/"guest.txt").write_text("\n".join(x.rstrip() for x in serial.log.decode().splitlines()).strip()+"\n")
        (opt.output/"physical.json").write_text(json.dumps(report,indent=2)+"\n")
print([(r["stage"],r["refs"],r["readers"]) for r in report["phases"]],report.get("at_close"))

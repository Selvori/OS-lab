"""Run guest checks and paired template experiments. Python standard library."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from bench_lifecycle import WARM
import mkfs
import test_lab4 as qemu

FIELDS = ("total setup close spawn reap user_peak kernel_peak copied_slots "
          "retains exit_slots bitmap_words private_drops resident_page_ticks "
          "first_write_ticks first_writes").split()
PRIMARY = (2048, 4, 512, "f")


def sample(vm, case, policy, n):
    pages, writes, stride, job = case
    cmd = f"tbench {policy} {pages} {writes} {stride} {n} {job}"
    vm.send(cmd.encode() + b"\r")
    output = vm.until(b"seele$ ",timeout=90).decode()
    rows = [line.split()[1:] for line in output.splitlines() if line.startswith("TB ")]
    if len(rows)!=1 or len(rows[0])!=len(FIELDS)+n or "USER FAULT" in output:
        raise ValueError(output)
    fields = list(map(int, rows[0]))
    return dict(zip(FIELDS, fields), rounds=fields[len(FIELDS):],
                case=list(case), policy=policy, n=n)


def collect(out, mode, smoke):
    directory = Path("build") / ("template-"+mode)
    apps = {name: directory/"apps"/(name+".bin") for name in
            ("sh","tcheck","tbench","tpin","lifejob")}
    subprocess.run(["make","-s","-j4",qemu.CONFIG,f"BUILD={directory}",
                    f"LIFECYCLE={mode}","build",*map(str,apps.values())],check=True)
    artifacts = dict(apps,kernel=directory/"kernel.elf")
    data = dict(artifacts={k:hashlib.sha256(v.read_bytes()).hexdigest()
                           for k,v in artifacts.items()}, samples=[],warmups=[])
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        image = Path(tmp).resolve()/"guest.img"
        mkfs.make_image(image,programs=list(apps.items()))
        os.environ.update(DISK=str(image), MAKEFLAGS=f"BUILD={directory} LIFECYCLE={mode}")
        qemu.OUT = out
        vm = qemu.Machine("template-"+mode)
        try:
            vm.until(b"seele$ ")
            if mode=="check":
                vm.send(b"tcheck\r")
                result=vm.until(b"seele$ ",timeout=120)
                assert b"TC PASS" in result and b"PANIC" not in result, result[-2400:]
                data["guest_checks"]="passed"
                data["pin_pages"]=[]
                for policy in range(3):
                    vm.send(f"tpin {policy}\r".encode())
                    block=vm.until(b"seele$ ",timeout=60).decode()
                    assert "PIN PASS" in block, block
                    row=next(line for line in block.splitlines() if line.startswith("PIN "))
                    data["pin_pages"].append(list(map(int,row.split()[1:])))
            else:
                cases=[(p,w,s,"e" if k=="exec" else "f") for p,w,s,k in WARM]
                cases += [(2048,0,1,"a"),(2048,0,1,"q"),(2048,0,1,"u")]
                for case in ([PRIMARY] if smoke else cases):
                    for policy in range(3):
                        data["warmups"].append(sample(vm,case,policy,2))
                    for rep in range(1 if smoke else 3):
                        for order in range(3):
                            data["samples"].append(sample(vm,case,(rep+order)%3,12))
                for n in (1,8,64):
                    for rep in range(1 if smoke else (12 if n==64 else 3)):
                        order=(0,1,2) if rep%2==0 else (2,1,0)
                        for policy in order:
                            row=sample(vm,PRIMARY,policy,n)
                            row["pair"]=rep
                            data["samples"].append(row)
        finally:
            vm.close()
            (out/(mode+".json")).write_text(json.dumps(data,separators=(",",":"))+"\n")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path)
    parser.add_argument("modes",nargs="+",choices=("check","counts","time"))
    parser.add_argument("--smoke",action="store_true")
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    for mode in args.modes:
        collect(args.output.resolve(),mode,args.smoke)

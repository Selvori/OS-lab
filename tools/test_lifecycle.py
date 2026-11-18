#!/usr/bin/env python3
"""Run lifecycle checks, historical regressions, and restore the formal builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

import mkfs
import test_lab4 as qemu

CASES = (
    "tree, first split, sole upgrade, partial unmap",
    "cross-page wait/read, bad address, second allocation rollback",
    "store OOM, three exec allocation failures, RX protection",
    "sparse cross-table, failed fork, shared frames",
)
ELFS = ("build/riscv64-qemu-virt-sbi/kernel.elf",
        "build/riscv64-visionfive2-uboot/kernel.elf")


def run(*args, env=None, log=None):
    if log is None:
        return subprocess.check_output(args, cwd=qemu.ROOT, env=env)
    with log.open("wb") as stream:
        subprocess.run(args, cwd=qemu.ROOT, env=env, stdout=stream,
                       stderr=subprocess.STDOUT, check=True)


def guest_checks(out, root):
    directory = Path("build/lifecycle-check")
    apps = [(name, directory / "apps" / (name + ".bin")) for name in ("sh", "lifetest", "lifejob")]
    run("make", "-j4", qemu.CONFIG, f"BUILD={directory}", "LIFECYCLE=check",
        "build", *(str(path) for _, path in apps), log=out / "check-build.log")
    image = root / "check.img"
    mkfs.make_image(image, programs=[(name, qemu.ROOT / path) for name, path in apps])
    names = ("MAKEFLAGS", "DISK")
    saved = {name: os.environ.get(name) for name in names}
    os.environ["MAKEFLAGS"] = f"BUILD={directory} LIFECYCLE=check"
    os.environ["DISK"] = str(image)
    qemu.OUT = out
    machine = None
    try:
        machine = qemu.Machine("lifecycle-cases")
        machine.until(b"seele$ ")
        for _ in range(8):
            machine.send(b"lifetest\r")
            block = machine.until(b"seele$ ", timeout=30).decode()
            assert "LIFE CHECK PASS\r\n" in block and "LIFE CHECK FAIL" not in block, block
            assert block.count("USER FAULT") == block.count("scause=0x000000000000000f") == 2, block
            assert all(f"LIFE CASE {case}\r\n" in block for case in CASES), block
        assert b"PANIC" not in machine.log
        return dict(repetitions=8, cases=list(CASES), expected_store_faults=16,
                    page_and_object_balances_checked=True)
    finally:
        try:
            if machine is not None:
                machine.close()
        finally:
            for name in names:
                if saved[name] is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = saved[name]


def tracked_source_hashes():
    sources = run("git", "ls-files", "-z", "arch", "include", "kernel", "mk", "user")
    return {name: hashlib.sha256((qemu.ROOT / name).read_bytes()).hexdigest()
            for name in sources.decode().strip("\0").split("\0")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/process-lifecycle/regression"))
    parser.add_argument("--guest-only", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    result = dict(failures=[])
    hashes = tracked_source_hashes()
    try:
        with tempfile.TemporaryDirectory(prefix="check-", dir=out) as tmp:
            root = Path(tmp)
            result["guest"] = guest_checks(out, root)
            if not args.guest_only:
                legacy = root / "legacy.img"
                mkfs.make_image(legacy)
                run("python3", "tools/test_lab6.py", "--extended",
                    env=dict(os.environ, DISK=str(legacy)), log=out / "lab6.log")
                result["lab6"] = json.loads((qemu.ROOT / "build/lab6-checks/results-extended.json").read_text())
                assert hashes == tracked_source_hashes()
                result["source_hashes_restored"] = True
                run("make", "-B", "-j4", qemu.CONFIG, "build", log=out / "qemu-build.log")
                run("make", "-B", "-j4", "CONFIG=riscv64-visionfive2-uboot", "build", "image",
                    log=out / "vf2-build.log")
                run("python3", "tools/test_lab9.py", log=out / "lab9.log")
                result["lab9"] = json.loads((qemu.ROOT / "build/lab9-checks/results.json").read_text())
                entries = []
                for elf, want in zip(ELFS, ("0x80200000", "0x40200000")):
                    header = run("riscv64-linux-gnu-readelf", "-h", elf).decode()
                    assert re.search(r"Entry point address:\s+" + want, header)
                    names = run("riscv64-linux-gnu-nm", elf)
                    assert not re.search(rb"lifecycle_|lab6_probe|lab6_fail", names)
                    entries.append(want)
                result["formal_entries"] = entries
                fit = run("dumpimage", "-l", "build/riscv64-visionfive2-uboot/kernel.itb")
                assert b"0x40200000" in fit
                (out / "vf2-fit.txt").write_bytes(fit)
                result["vf2_fit"] = True
    except Exception as error:
        result["failures"].append(repr(error))
        raise
    finally:
        (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()

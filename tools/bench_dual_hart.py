#!/usr/bin/env python3
"""Paired boots and a separate, stopped-guest GDB privilege witness."""
import argparse
import csv
import hashlib
import os
from pathlib import Path
import re
import socket
import subprocess
import time

import mkfs
import test_lab4 as qemu


def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def make(directory, dual, lifecycle, *targets):
    flags = (f"BUILD={directory}", f"DUAL_HART_USERS={int(dual)}",
             f"LIFECYCLE={lifecycle}")
    subprocess.run(["make", "-s", "-j4", qemu.CONFIG, *flags, *targets], check=True)
    return " ".join(flags)


def checksum(iterations):
    """Exponentiate the affine recurrence independently in Python."""
    scale, offset = 1664525, 1013904223
    value = 1
    while iterations:
        if iterations & 1:
            value = (scale * value + offset) & 0xffffffff
        offset = ((scale + 1) * offset) & 0xffffffff
        scale = (scale * scale) & 0xffffffff
        iterations >>= 1
    return value & 0x7fffffff


def boot(out, label, flags, image, port=None):
    os.environ.update(MAKEFLAGS=flags, DISK=str(image.resolve()))
    emulator = ("QEMU=qemu-system-riscv64 -machine virt -bios default "
                "-accel tcg,thread=multi -m 128M -smp 2 -nographic")
    if port:
        emulator += f" -gdb tcp:127.0.0.1:{port}"
    qemu.OUT = out
    return qemu.Machine(label, (emulator,))


def guest_start(machine, iterations):
    machine.send(f"dualbench {iterations}\r".encode())
    machine.until(b"DUAL START\r\n")


def guest_result(machine, iterations):
    output = machine.until(b"seele$ ", timeout=30)
    row = re.search(rb"DUAL PASS (\d+) (\d+) (\d+) (\d+)\r\n", output)
    assert row and b"PANIC" not in machine.log and b"USER FAULT" not in output, output
    count, parent, child, ticks = map(int, row.groups())
    assert count == iterations and parent == child == checksum(iterations) and ticks > 0, output
    return dict(iterations=count, parent=parent, child=child, time_ticks=ticks)


def boot_owner(machine):
    output = machine.until(b"seele$ ")
    owner = re.search(rb"owner cpu=(\d+)", output)
    assert owner and output.count(b"UART external interrupt route ready") == 2, output
    return int(owner[1])


def inspect(out, label, port, elf, code_range, dual):
    first, last = code_range
    # Each GDB attachment stops the entire guest. Detach resumes both harts.
    # Several attempts tolerate sampling exactly on a timer or IPI boundary.
    for _ in range(8):
        registers = command("gdb-multiarch", "-nx", "-batch", str(elf),
                            "-ex", f"target remote 127.0.0.1:{port}",
                            "-ex", "thread apply all info registers mhartid pc priv satp",
                            "-ex", "monitor info cpus",
                            "-ex", "detach")
        with (out / f"{label}-gdb.txt").open("a") as log:
            log.write(registers)
        rows = [tuple(int(value, 16) for value in row) for row in re.findall(
            r"(?m)^mhartid\s+(\S+)[^\n]*\n"
            r"pc\s+(\S+)[^\n]*\npriv\s+(\S+)[^\n]*\nsatp\s+(\S+)", registers)]
        threads = {int(cpu): int(tid) for cpu, tid in re.findall(
            r"CPU #(\d+): thread_id=(\d+)", registers)}
        assert set(threads) == {0, 1} and len(set(threads.values())) == 2, registers
        assert {row[0] for row in rows} == {0, 1}, registers
        active = [row for row in rows if first <= row[1] < last and row[2] == 0]
        if len(active) == (2 if dual else 1):
            if dual:
                assert len({row[3] for row in active}) == 2, registers
            return [(*row, threads[row[0]]) for row in rows]
    raise AssertionError(f"Missing privilege witness for {label}")


def run_bench(out, pairs, iterations, witness_only):
    out.mkdir(parents=True, exist_ok=True)
    flags, kernels = {}, {}
    apps = out / "apps-build"
    programs = ("sh", "dualbench")
    paths = {name: apps / "apps" / f"{name}.bin" for name in programs}
    app_elf = apps / "apps/dualbench.elf"
    make(apps, False, "time", *map(str, paths.values()), str(app_elf))
    sym = re.search(r"(?m)^([0-9a-f]+) ([0-9a-f]+) t calculate$",
                    command("riscv64-linux-gnu-nm", "-S", str(app_elf)))
    assert sym
    first, size = (int(field, 16) for field in sym.groups())
    image = out / "bench.img"
    mkfs.make_image(image, programs=paths.items())
    digests = {name: hashlib.sha256(path.read_bytes()).hexdigest()
               for name, path in {**paths, "disk": image}.items()}
    for mode in (1, 2):
        directory = out / f"kernel-{mode}-time"
        flags[mode] = make(directory, mode == 2, "time", "build")
        kernels[mode] = directory / "kernel.elf"
        digests[f"kernel-{mode}"] = hashlib.sha256(kernels[mode].read_bytes()).hexdigest()
    (out / "artifacts.txt").write_text("".join(f"{digest}  {name}\n" for name, digest
                                              in digests.items()))
    if not witness_only:
        with (out / "paired.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, ("pair", "position", "mode", "owner",
                                              "iterations", "parent", "child", "time_ticks"))
            writer.writeheader()
            for pair in range(1, pairs + 1):
                for position, mode in enumerate((1, 2) if pair % 2 else (2, 1), 1):
                    machine = boot(out, f"pair-{pair}-mode-{mode}", flags[mode], image)
                    try:
                        owner = boot_owner(machine)
                        guest_start(machine, iterations)
                        guest_result(machine, iterations)  # Unrecorded warmup.
                        guest_start(machine, iterations)
                        row = guest_result(machine, iterations)
                        writer.writerow(dict(pair=pair, position=position, mode=mode,
                                             owner=owner, **row))
                        stream.flush()
                        print(pair, mode, row, flush=True)
                    finally:
                        machine.close()

    with (out / "witness.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("mode", "owner", "hart", "user_loop_start", "user_loop_end", "pc",
                         "priv", "satp", "host_tid"))
        for mode in (1, 2):
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                port = listener.getsockname()[1]
            label = f"witness-mode-{mode}"
            (out / f"{label}-gdb.txt").write_text("")
            machine = boot(out, label, flags[mode], image, port)
            try:
                owner = boot_owner(machine)
                guest_start(machine, 500_000_000)
                time.sleep(0.05)
                for hart, pc, priv, satp, tid in inspect(out, label, port, kernels[mode],
                                                   (first, first + size), mode == 2):
                    writer.writerow((mode, owner, hart, hex(first), hex(first + size),
                                     hex(pc), priv, hex(satp), tid))
                stream.flush()
                guest_result(machine, 500_000_000)
            finally:
                machine.close()


def run_smoke(out):
    out.mkdir(parents=True, exist_ok=True)
    directory = out / "kernel-2-check"
    programs = ("sh", "lifetest", "lifejob", "bcheck", "bclose")
    paths = {name: directory / "apps" / f"{name}.bin" for name in programs}
    flags = make(directory, True, "check", "build", *map(str, paths.values()))
    image = out / "smoke.img"
    mkfs.make_image(image, programs=paths.items())
    machine = boot(out, "smoke", flags, image)
    try:
        boot_owner(machine)
        for _ in range(8):
            machine.send(b"lifetest\r")
            output = machine.until(b"seele$ ", timeout=30)
            assert b"LIFE CHECK PASS" in output and output.count(b"USER FAULT") == 2, output
        for check, expected in ((b"bcheck 9", b"CHECK PASS"), (b"bclose 9 idle", b"CLOSE PASS")):
            machine.send(check + b"\r")
            output = machine.until(b"seele$ ", timeout=30)
            assert expected in output, output
        assert b"PANIC" not in machine.log
    finally:
        machine.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--witness-only", action="store_true")
    modes.add_argument("--smoke", action="store_true")
    parser.add_argument("--pairs", type=int, default=4)
    parser.add_argument("--iterations", type=int, default=128_000_000)
    args = parser.parse_args()
    if args.pairs < 1 or not 1 <= args.iterations <= 900_000_000:
        parser.error("positive pairs and 1–900,000,000 iterations required")
    if args.smoke:
        run_smoke(args.output)
    else:
        run_bench(args.output, args.pairs, args.iterations, args.witness_only)

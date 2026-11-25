#!/usr/bin/env python3
"""Paired template timing, ownership checks and separate GDB witnesses."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import time

from bench_dual_hart import boot_owner, command, make
from bench_window_reclaim import lines
import mkfs
import test_lab4 as qemu

BASELINE = "9e3a6535bef19eb7aee68122a49f9b9b6bbeed7a"
TICKS = ("spawn1", "spawn2", "both_waited", "close", "close_to_wait",
         "post_wait_drain", "close_to_drained", "queue_after_wait")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


QEMU = ("qemu-system-riscv64", "-machine", "virt", "-bios", "default",
        "-accel", "tcg,thread=multi", "-m", "128M", "-smp", "2", "-nographic",
        "-global", "virtio-mmio.force-legacy=false")


def boot(out, label, elf, disk, port=None):
    argv = [*QEMU, "-drive", f"file={disk},if=none,format=raw,id=seele-disk",
            "-device", "virtio-blk-device,drive=seele-disk", "-kernel", str(elf)]
    if port is not None:
        argv += ["-gdb", f"tcp:127.0.0.1:{port}"]
    qemu.OUT = out
    return qemu.Machine(label, argv=argv)


def read_rows(output, key, width):
    matches = re.findall(rf"(?m)^{key} ([0-9 ]+)\r?$", output)
    rows = [list(map(int, match.split())) for match in matches]
    assert rows and all(len(row) == width for row in rows), output
    return rows


def checksum(iterations, pid):
    scale, offset = 1664525, 1013904223
    while iterations:
        if iterations & 1:
            pid = (scale * pid + offset) & 0xffffffff
        scale, offset = scale * scale & 0xffffffff, (scale + 1) * offset & 0xffffffff
        iterations >>= 1
    return pid & 0x7fffffff


def result(output, policy, iterations):
    assert "PAIR PASS" in output and "PAIR FAIL" not in output and "FAIL " not in output
    assert "PANIC" not in output and "USER FAULT" not in output, output
    (reported, count, a, b, w1, s1, w2, s2), = read_rows(output, "PAIR RESULT", 8)
    assert (reported, count, b) == (policy, iterations, a + 1)
    assert {w1, w2} == {a, b}
    assert (s1, s2) == (checksum(iterations, w1), checksum(iterations, w2))
    ticks, = read_rows(output, "PAIR TICKS", len(TICKS))
    return dict(policy=policy, iterations=iterations, spawns=[a, b], exits=[[w1, s1], [w2, s2]],
                ticks=dict(zip(TICKS, ticks)))


def bopen(machine, policy, rounds):
    output = lines(machine, f"bopen {policy} {rounds} 512")
    assert "OPEN PASS" in output and "USER FAULT" not in output, output
    fields = ("policy", "rounds", "stride", "freeze", "spawn_sum", "close", "drain",
              "freeze_to_drained", "reap_sum", "copied_slots", "user_peak", "kernel_peak")
    row, = read_rows(output, "OPEN", len(fields))
    samples = read_rows(output, "ROUND", 2)
    assert row[:3] == [policy, rounds, 512] and [i for i, _ in samples] == list(range(rounds))
    return dict(zip(fields, row), spawn_to_wait=[ticks for _, ticks in samples])


def paired(out, kernels, disk, iterations, pairs, artifacts):
    rows = []
    for pair in range(1, pairs + 1):
        policies = (1, 9) if pair % 2 else (9, 1)
        variants = ("base", "parallel") if pair % 2 else ("parallel", "base")
        for position, variant in enumerate(variants, 1):
            assert digest(disk) == artifacts["disk"]
            elf = kernels[variant]
            role = "kernel_base" if variant == "base" else "kernel_parallel"
            assert digest(elf) == artifacts[role]
            machine = boot(out, f"pair-{pair}-{variant}", elf, disk)
            try:
                owner = boot_owner(machine)
                warmup = [result(lines(machine, f"tdbench {policy} {iterations}"),
                                 policy, iterations) for policy in policies]
                measurements = [result(lines(machine, f"tdbench {policy} {iterations}"),
                                       policy, iterations) for policy in policies]
                serial = [bopen(machine, policy, 16) for policy in policies]
            finally:
                machine.close()
            assert digest(elf) == artifacts[role]
            rows.append(dict(pair=pair, position=position, variant=variant, owner=owner,
                             warmup=warmup, measured=measurements, serial_bopen=serial))
            (out / "paired.json").write_text(json.dumps(dict(artifacts=artifacts, boots=rows),
                                                        indent=2) + "\n")
            print(pair, position, variant, owner, [m["ticks"]["both_waited"]
                                                   for m in measurements], flush=True)
    assert digest(disk) == artifacts["disk"]


# GDB attaches all-stop. Match live satp roots to actual sibling PCBs and their
# same frozen image, rather than deducing activity merely from per-hart counters.
SNAPSHOT = """\
thread apply all info registers mhartid pc priv satp
monitor info cpus
maintenance packet Qqemu.PhyMemMode:1
set $i = 0
while $i < sizeof(procs) / sizeof(procs[0])
  if procs[$i].branch
    set $p = &procs[$i]
    set $image = $p->branch->image
    set $remaining = -1
    set $closed = $p->parent->template_handle == 0
    if $image->reclaim_job
      set $remaining = (int)$image->reclaim_job->remaining
      set $closed = $image->reclaim_job->closed
    end
    printf "BRANCH %d %d %d 0x%lx 0x%lx %d %d %d %d %d 0x%x\\n", $p->pid, $p->parent->pid, $p->state, $p->pgtbl, $image->pgtbl, $image->state, $image->template_branches, $image->template_refs, $closed, $remaining, $p->branch->shared
    if $remaining == 10
      set $w = 0
      while $w < sizeof($p->branch->dropped) / sizeof($p->branch->dropped[0])
        set $word = 0
        while $word < 8
          set $original = $image->reclaim_job->original[$w][$word]
          if $original
            printf "WORD %d %d %d 0x%lx 0x%lx 0x%lx\\n", $p->pid, $w, $word, $original, $image->reclaim_job->held[$w][$word], $p->branch->dropped[$w][$word]
          end
          set $word = $word + 1
        end
        set $w = $w + 1
      end
    end
  end
  set $i = $i + 1
end
detach
"""


def pin_masks(capture, pids):
    held = {pid: set() for pid in pids}
    borrowed = {pid: set() for pid in pids}
    words = [
        [int(v, 0) for v in line.split()[1:]]
        for line in capture.splitlines() if line.startswith("WORD ")]
    for pid, window, word, original, remaining, dropped in words:
        for bit in range(64):
            page = window * 512 + word * 64 + bit - 3
            if remaining & (1 << bit):
                held[pid].add(page)
            if (original & ~dropped) & (1 << bit):
                borrowed[pid].add(page)
    for pid in pids:
        assert borrowed[pid] == {i + (pid & 1) for i in (0, 509, 1021, 1533, 2045)}
        assert held[pid] == borrowed[pids[0]] | borrowed[pids[1]]
    return {pid: sorted(pages) for pid, pages in borrowed.items()}


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def witness(out, disk, elf, app, variant, policy, pin=False):
    sym = re.search(r"(?m)^([0-9a-f]+) ([0-9a-f]+) t calculate$",
                    command("riscv64-linux-gnu-nm", "-S", str(app)))
    assert sym
    first, size = (int(field, 16) for field in sym.groups())
    iterations = 600_000_000
    label = f"witness-{variant}-{policy}" + ("-pin" if pin else "")
    port = free_port()
    script = out / f"{label}.gdb"
    script.write_text(SNAPSHOT)
    machine = boot(out, label, elf, disk, port)
    try:
        owner = boot_owner(machine)
        machine.send((f"tdbench {policy} {iterations}" + (" pin" if pin else "") + "\r").encode())
        prefix = machine.until(b"\r\nPAIR PIN ") if pin else machine.until(b"PAIR START\r\n")
        if not pin:
            time.sleep(0.045)
        raw = out / f"{label}-gdb.txt"
        for _ in range(8):
            snapshot = subprocess.run(
                ["gdb-multiarch", "-nx", "-batch", str(elf), "-ex",
                 f"target remote 127.0.0.1:{port}", "-x", str(script)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=12)
            capture = snapshot.stdout
            with raw.open("a") as log:
                log.write(capture)
            assert snapshot.returncode == 0, capture
            registers = [tuple(int(v, 16) for v in row) for row in re.findall(
                r"(?m)^mhartid\s+(\S+)[^\n]*\npc\s+(\S+)[^\n]*\n"
                r"priv\s+(\S+)[^\n]*\nsatp\s+(\S+)", capture)]
            threads = {int(h): int(tid) for h, tid in re.findall(
                r"CPU #(\d+): thread_id=(\d+)", capture)}
            assert {r[0] for r in registers} == set(threads) == {0, 1}
            assert len(set(threads.values())) == 2
            branches = [list(map(lambda v: int(v, 0), line.split()[1:]))
                        for line in capture.splitlines() if line.startswith("BRANCH ")]
            if len(branches) != 2:
                continue
            a, b = branches
            assert a[1] == b[1] and a[3] != b[3] and a[4] == b[4]
            assert a[4] not in (a[3], b[3]) and a[5] == b[5] == 1  # PROC_USED
            active = [r for r in registers if first <= r[1] < first + size and r[2] == 0]
            required = 1 if variant == "base" else 2
            if len(active) != required or not (a[8] == b[8] == 1):
                continue
            roots = {row[3] for row in branches}
            assert all(((satp & ((1 << 44) - 1)) << 12) in roots
                       for _, _, _, satp in active)
            assert a[6] == b[6] == 2 and a[7] == b[7] == (2 if policy == 1 else 3)
            if required == 2:
                assert {a[2], b[2]} == {3, 6}  # PROC_RUNNING and PROC_REMOTE
                assert len({r[3] for r in active}) == 2
            pins = None
            if pin:
                assert a[9] == b[9] == 10 and a[10] == b[10] == 0
                pins = pin_masks(capture, [a[0], b[0]])
            output = prefix + machine.until(b"seele$ ", timeout=30)
            passed = result(output.decode(), policy, iterations)
            return dict(label=label, owner=owner, branches=branches, threads=threads,
                        code_range=[hex(first), hex(first + size)],
                        harts=[[h, hex(pc), priv, hex(satp)] for h, pc, priv, satp in registers],
                        completion=passed, pinned_indices=pins)
        raise AssertionError(f"No qualifying GDB capture: {label}")
    finally:
        machine.close()



HANDOFF = r"""set pagination off
maintenance packet Qqemu.PhyMemMode:1
tbreak worker_trap if $scause == 15 && $stval >= 0x3000 && $stval < 0x803000 && remote_proc && remote_proc->branch
printf "LEASE ARMED\n"
continue
set $tf = remote_proc->tf
set $fault = $stval
set $page = $fault & -4096
printf "LEASE WORKER %ld 0x%lx 0x%lx 0x%lx\n", $mhartid, $satp, $fault, $tf
tbreak trap_user_resume_remote if current[scheduler_owner] && current[scheduler_owner]->tf == $tf
continue
set $proc = current[scheduler_owner]
printf "LEASE REPLAY %ld 0x%lx %d %d 0x%lx 0x%lx 0x%lx\n", $mhartid, $satp, $proc->pid, $proc->state, $proc->remote_cause, $proc->remote_value, $proc->tf
tbreak branch_drop_borrow if p == $proc && va == $page
continue
set $branch = p->branch
set $window = va >> 21
set $slot = (va >> 12) & 511
set $word = $slot >> 6
set $mask = 1ULL << ($slot & 63)
printf "LEASE BEFORE %ld %d %d %d\n", $mhartid, $proc->pid, $branch->image->reclaim_job->closed, !!($branch->dropped[$window][$word] & $mask)
finish
printf "LEASE AFTER %ld %d %d\n", $mhartid, $proc->pid, !!($branch->dropped[$window][$word] & $mask)
detach
"""


def fault_replay(out, disk, elf):
    """Observe the same borrowed leaf at worker trap, owner replay and drop."""
    label = "remote-cow"
    script = out / f"{label}.gdb"
    script.write_text(HANDOFF)
    raw = out / f"{label}-gdb.txt"
    port = free_port()
    machine = boot(out, label, elf, disk, port)
    iterations = 600_000_000
    try:
        owner = boot_owner(machine)
        with raw.open("wb") as log:
            argv = ["gdb-multiarch", "-nx", "-batch", str(elf),
                    "-ex", f"target remote 127.0.0.1:{port}", "-x", str(script)]
            with subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT) as gdb:
                try:
                    deadline = time.monotonic() + 5
                    while b"LEASE ARMED" not in raw.read_bytes():
                        assert gdb.poll() is None and time.monotonic() < deadline, raw.read_text()
                        time.sleep(0.02)
                    machine.send(f"tdbench 9 {iterations} pin\r".encode())
                    assert gdb.wait(timeout=10) == 0, raw.read_text()
                finally:
                    if gdb.poll() is None:
                        gdb.kill()
        capture = raw.read_text()
        evidence = {row[1]: [int(field, 0) for field in row[2:]]
                    for row in (line.split() for line in capture.splitlines())
                    if len(row) > 2 and row[0] == "LEASE"}
        worker, satp, va, tf = evidence["WORKER"]
        replay, replay_satp, pid, state, cause, stval, replay_tf = evidence["REPLAY"]
        assert worker == 1 - owner and replay == owner and satp == replay_satp
        assert satp >> 60 == 8 and state == 3 and cause == 15 and (va, tf) == (stval, replay_tf)
        assert evidence["BEFORE"] == [owner, pid, 1, 0]
        assert evidence["AFTER"] == [owner, pid, 1]
        completion = result(machine.until(b"seele$ ", timeout=30).decode(), 9, iterations)
        assert pid in completion["spawns"]
        return dict(owner=owner, evidence=evidence, completion=completion)
    finally:
        machine.close()


def verify(out, disk, elf, app):
    machine = boot(out, "checks", elf, disk)
    try:
        owner = boot_owner(machine)
        for cmd, marker in (("bcheck 9", "CHECK PASS"), ("bclose 9 idle", "CLOSE PASS"),
                            ("byield ref9", "OWNER PASS"), ("byield sparse9", "SPARSE PASS"),
                            ("bedges rotation", "EDGES PASS"), ("bfail 6", "FAILURE PASS")):
            output = lines(machine, cmd)
            assert marker in output and "USER FAULT" not in output, output
        for _ in range(8):
            output = lines(machine, "lifetest")
            assert "LIFE CHECK PASS" in output and output.count("USER FAULT") == 2
        checks = [result(lines(machine, f"tdbench {policy} 26000000 check"), policy, 26000000)
                  for policy in (1, 9)]
    finally:
        machine.close()
    pin = witness(out, disk, elf, app, "parallel", 9, pin=True)
    handoff = fault_replay(out, disk, elf)
    (out / "checks.json").write_text(json.dumps(dict(owner=owner, checks=checks, pin=pin,
                                                    remote_cow=handoff), indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("baseline_elf", type=Path)
    parser.add_argument("mode", choices=("time", "witness", "check"))
    parser.add_argument("--pairs", type=int, default=4)
    parser.add_argument("--iterations", type=int, default=128_000_000)
    args = parser.parse_args()
    if args.pairs < 1 or not 1 <= args.iterations <= 900_000_000:
        parser.error("positive pairs and 1–900,000,000 iterations required")
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    base = args.baseline_elf
    mode = "check" if args.mode == "check" else "time"
    apps = out / "apps-build"
    programs = ["sh", "tdbench", "bopen"]
    if mode == "check":
        programs += ["bcheck", "bclose", "byield", "bedges", "bfail", "lifetest", "lifejob"]
    paths = {name: apps / "apps" / f"{name}.bin" for name in programs}
    app = apps / "apps/tdbench.elf"
    make(apps, False, mode, *map(str, paths.values()), str(app))
    parallel = out / f"parallel-{mode}"
    make(parallel, True, mode, "build")
    disk = out / "guest.img"
    mkfs.make_image(disk, programs=paths.items())
    elf = parallel / "kernel.elf"
    artifacts = {name: digest(path) for name, path in {**paths, "disk": disk,
                                                      "kernel_parallel": elf}.items()}
    artifacts.update(baseline=BASELINE, candidate=command("git", "rev-parse", "HEAD").strip(),
                     source_clean=not command("git", "status", "--porcelain").strip(),
                     qemu=command("qemu-system-riscv64", "--version").splitlines()[0],
                     gcc=command("riscv64-linux-gnu-gcc", "--version").splitlines()[0],
                     timebase_hz=10_000_000, cpus=2, accel="tcg,thread=multi")
    if args.mode != "check":
        artifacts["kernel_base"] = digest(base)
        assert artifacts["kernel_base"] != artifacts["kernel_parallel"], "identical ELF files"
        assert "branch_can_offload" not in command("riscv64-linux-gnu-nm", str(base))
    (out / "artifacts.json").write_text(json.dumps(artifacts, indent=2) + "\n")
    if args.mode == "time":
        paired(out, {"base": base, "parallel": elf}, disk, args.iterations, args.pairs, artifacts)
    elif args.mode == "witness":
        captures = [witness(out, disk, path, app, variant, policy)
                    for variant, path, policy in (("base", base, 9),
                                                  ("parallel", elf, 1), ("parallel", elf, 9))]
        (out / "witness.json").write_text(json.dumps(captures, indent=2) + "\n")
    else:
        verify(out, disk, elf, app)
    assert digest(elf) == artifacts["kernel_parallel"]
    if args.mode != "check":
        assert digest(base) == artifacts["kernel_base"]


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Temporary lock/frame counters for ordinary heap clone and destruction."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path.cwd() / "tools"))
import bench_lifecycle as bench
import mkfs

out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
version = sys.argv[2]
sources = {Path(name): Path(name).read_bytes() for name in (
    "include/kernel/lifecycle_probe.h", "kernel/proc/lifecycle_probe.c",
    "kernel/mm/page.c", "kernel/mm/user.c",
)}

def replace(name, old, new):
    p = Path(name)
    text = p.read_text()
    assert text.count(old) == 1, (p, old)
    p.write_text(text.replace(old, new))

hdr, control, alloc, vm = map(str, sources)
fmt = ("retain_frames", "retain_locks", "release_frames", "release_locks",
       "publish_frames", "publish_locks")
cases = [(16, 0, 1, "fork"), (33, 1, 1, "fork"), (256, 0, 1, "fork"),
         (2048, 0, 1, "fork"), (2048, 4, 512, "fork"),
         (2048, 2048, 1, "fork"), (2048, 0, 1, "exec")]
try:
    assert not subprocess.check_output(["git", "status", "--porcelain"])
    commit = bench.command("git", "rev-parse", "HEAD")
    replace(hdr, "void lifecycle_page(bool user, bool allocated);", """struct heap_ref_trace {
    unsigned """ + ", ".join(fmt) + """;
};
extern struct heap_ref_trace heap_ref_trace;
#define heap_ref_add(field, n) do { if (lifecycle_active) heap_ref_trace.field += (n); } while (0)
void lifecycle_page(bool user, bool allocated);""")
    replace(hdr, "#define lifecycle_page(user, allocated) ((void)0)",
            "#define heap_ref_add(field, n) ((void)0)\n#define lifecycle_page(user, allocated) ((void)0)")
    replace(control, "bool lifecycle_in_heap;", "bool lifecycle_in_heap;\nstruct heap_ref_trace heap_ref_trace;")
    replace(control, "        extra_pages = 0;",
            "        heap_ref_trace = (struct heap_ref_trace){0};\n        extra_pages = 0;")
    replace(control, "    if (lifecycle_active && op != PROBE_BALANCE) account_residency();",
            """    if (lifecycle_active && op != PROBE_BALANCE) account_residency();
    if (op == PROBE_STOP) {
        printf("HEAP REFS %d %d %d %d %d %d\\n",
               """ + ",\n               ".join("(int)heap_ref_trace." + f for f in fmt) + """);
    }""")
    if version == "baseline":
        # Ordinary clone uses exactly one allocator-lock entry per scalar retain.
        replace(vm, "            page_retain_user((void *)pa);",
                """            page_retain_user((void *)pa);
            heap_ref_add(retain_frames, 1);
            heap_ref_add(retain_locks, 1);""")
        replace(alloc, '        assert(*refs != 0, "release idle user frame");',
                """#ifdef LIFECYCLE_COUNTS
        if (lifecycle_in_heap) {
            heap_ref_add(release_frames, 1);
            heap_ref_add(release_locks, 1);
        }
#endif
        assert(*refs != 0, "release idle user frame");""")
        replace(alloc, "    zone->free_count++;\n    spinlock_release(&zone->lock);",
                """    zone->free_count++;
#ifdef LIFECYCLE_COUNTS
    if (zone == &zones[PAGE_ZONE_USER] && lifecycle_in_heap) {
        heap_ref_add(publish_frames, 1);
        heap_ref_add(publish_locks, 1);
    }
#endif
    spinlock_release(&zone->lock);""")
    else:
        replace(alloc, "    size_t last = 0;", """    size_t last = 0;
    heap_ref_add(release_frames, count);
    heap_ref_add(release_locks, 1);""")
        replace(alloc, "    zone->free_count += last;", """    zone->free_count += last;
    heap_ref_add(publish_frames, last);
    heap_ref_add(publish_locks, 1);""")
        replace(alloc, """    spinlock_acquire(&zone->lock);
    for (size_t i = 0; i < count; ++i) {
        uint16_t *refs = user_ref((uintptr_t)pages[i]);
        assert(*refs && *refs < UINT16_MAX, "retain invalid user frame");""",
                """    spinlock_acquire(&zone->lock);
    heap_ref_add(retain_frames, count);
    heap_ref_add(retain_locks, 1);
    for (size_t i = 0; i < count; ++i) {
        uint16_t *refs = user_ref((uintptr_t)pages[i]);
        assert(*refs && *refs < UINT16_MAX, "retain invalid user frame");""")
    directory = Path("build") / f"lifecycle-ref-probe-{version}"
    programs = [(name, directory / "apps" / (name + ".bin"))
                for name in ("sh", "lifebench", "lifejob")]
    subprocess.run(["make", "-B", "-j4", bench.qemu.CONFIG, f"BUILD={directory}",
                    "LIFECYCLE=counts", "build", *[str(p) for _, p in programs]],
                   check=True, stdout=subprocess.DEVNULL)
    env = {k: os.environ.get(k) for k in ("MAKEFLAGS", "DISK")}
    machine = None
    records = []
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        image = Path(tmp) / "probe.img"
        mkfs.make_image(image, programs=programs)
        os.environ["MAKEFLAGS"] = f"BUILD={directory} LIFECYCLE=counts"
        os.environ["DISK"] = str(image)
        bench.qemu.OUT = out
        try:
            machine = bench.qemu.Machine("refs-" + version)
            machine.until(b"seele$ ")
            for case in cases:
                mark = len(machine.log)
                bench.run_case(machine, case, 2)
                lines = re.findall(rb"^HEAP REFS ([0-9 ]+)\r?$", machine.log[mark:], re.M)
                assert len(lines) == 2, (case, machine.log[mark:])
                rows = [dict(zip(fmt, map(int, line.split()))) for line in lines]
                assert all(len(row) == len(fmt) for row in rows)
                assert rows[0] == rows[1], (case, rows)
                n, writes, _, job = case
                for row in rows:
                    assert row["retain_frames"] == row["release_frames"] == n
                    assert row["publish_frames"] == writes
                    assert 0 < row["retain_locks"] <= n
                    assert 0 < row["release_locks"] <= n
                    assert 0 <= row["publish_locks"] <= writes
                records.append(dict(case=case, rows=rows))
            assert b"PANIC" not in machine.log
        finally:
            try:
                if machine:
                    machine.close()
            finally:
                for k, v in env.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v
    report = dict(commit=commit, version=version, records=records,
                  programs={n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in programs})
    (out / f"refs-{version}.json").write_text(json.dumps(report, indent=2) + "\n")
finally:
    for path, raw in sources.items():
        path.write_bytes(raw)
    assert all(path.read_bytes() == raw for path, raw in sources.items())
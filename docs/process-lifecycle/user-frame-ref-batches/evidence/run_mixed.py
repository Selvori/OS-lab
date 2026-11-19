#!/usr/bin/env python3
"""Compile a temporary minimal guest to check mixed releases and fork rollback."""
import json
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path.cwd() / "tools"))
import bench_lifecycle as bench
import mkfs
from test_lifecycle import run

out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = Path("tools/lifecycle_cases.c")
original = source.read_bytes()
code = Path(sys.argv[2]).read_text()
directory = Path("build/lifecycle-ref-batch-check")
programs = [(n, directory / "apps" / (n + ".bin")) for n in ("sh", "lifetest")]
try:
    fixture = original.decode().split("static void tree(void)")[0]
    start = fixture.index("static void pause_for(")
    fixture = fixture[:start] + fixture[fixture.index("\n", start):]
    start = fixture.index("static bool same_mapping(")
    fixture = fixture[:start] + fixture[fixture.index("static void resources(", start):]
    source.write_text(fixture + code)
    run("make", "-B", "-j4", bench.qemu.CONFIG, f"BUILD={directory}",
        "LIFECYCLE=check", "build", *[str(p) for _, p in programs], log=out / "mixed-build.log")
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        disk = Path(tmp) / "batch-check.img"
        mkfs.make_image(disk, programs=programs)
        os.environ.update(MAKEFLAGS=f"BUILD={directory} LIFECYCLE=check", DISK=str(disk))
        bench.qemu.OUT = out
        machine = bench.qemu.Machine("mixed")
        try:
            machine.until(b"seele$ ")
            for _ in range(8):
                machine.send(b"lifetest\r")
                block = machine.until(b"seele$ ")
                assert b"HEAP BATCH CHECK PASS\r\n" in block and b"LIFE CHECK FAIL" not in block, block
            assert b"PANIC" not in machine.log and b"USER FAULT" not in machine.log
        finally:
            machine.close()
finally:
    source.write_bytes(original)
    assert source.read_bytes() == original

report = dict(repetitions=8, commit=bench.command("git", "rev-parse", "HEAD"),
              heap_pages=35, injected_failure_after_allocations=8,
              restored_parent_writable_flags=True, restored_resource_counts=True,
              mixed_batch_shared=31, mixed_batch_last=1, tail_pages=1,
              source_restored=True)
(out / "mixed.json").write_text(json.dumps(report, indent=2) + "\n")

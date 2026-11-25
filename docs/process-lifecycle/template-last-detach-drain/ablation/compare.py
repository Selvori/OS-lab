"""Four configurations in balanced orders, using only the signed short workload."""
import argparse
import json
from pathlib import Path
from statistics import median
import sys

sys.path.insert(0, str(Path.cwd() / "tools"))
import bench_template_dual_hart as bench

parser = argparse.ArgumentParser(description=__doc__)
for argument in ("old", "terminal", "donor", "both", "disk", "out"):
    parser.add_argument(argument, type=Path)
args = parser.parse_args()
kernels = {key: getattr(args, key) for key in ("old", "terminal", "donor", "both")}
hashes = {key: bench.digest(path) for key, path in kernels.items()}
disk_hash = bench.digest(args.disk)
assert len(set(hashes.values())) == 4
args.out.mkdir(parents=True, exist_ok=True)
orders = (
    ("old", "terminal", "both", "donor"),
    ("terminal", "donor", "old", "both"),
    ("donor", "both", "terminal", "old"),
    ("both", "old", "donor", "terminal"),
)
boots = []
for block, order in enumerate(orders, 1):
    for variant in order:
        kernel = kernels[variant]
        assert bench.digest(kernel) == hashes[variant] and bench.digest(args.disk) == disk_hash
        machine = bench.boot(args.out, f"block-{block}-{variant}", kernel, args.disk)
        try:
            owner = bench.boot_owner(machine)
            readings = [bench.result(bench.lines(machine, "tdbench 9 1"), 9, 1) for _ in range(4)]
        finally:
            machine.close()
        assert bench.digest(kernel) == hashes[variant] and bench.digest(args.disk) == disk_hash
        ticks = median(row["ticks"]["post_wait_drain"] for row in readings[1:])
        boots.append(dict(block=block, variant=variant, owner=owner, warmup=readings[0],
                          measured=readings[1:], tail_ms=ticks / 10000))
medians = {v: median(b["tail_ms"] for b in boots if b["variant"] == v) for v in kernels}
report = dict(kernels=hashes, disk_sha256=disk_hash, tail_ms=medians, boots=boots)
(args.out / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
print(medians)

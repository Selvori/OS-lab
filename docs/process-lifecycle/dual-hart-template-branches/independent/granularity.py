"""Same 8 MiB two-branch guest, with little arithmetic or two million iterations."""
import argparse
import json
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path.cwd() / "tools"))
import bench_template_dual_hart as bench

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("manifest_dir", type=Path, help="completed signed-code timing run, including guest.img")
ap.add_argument("baseline_elf", type=Path)
ap.add_argument("output", type=Path)
args = ap.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
manifest=json.loads((args.manifest_dir / "artifacts.json").read_text())
elf = {"base": args.baseline_elf, "parallel": args.manifest_dir / "parallel-time/kernel.elf"}
disk=args.manifest_dir/"guest.img"
def verify():
    for variant, path in elf.items():
        assert bench.digest(path) == manifest["kernel_" + variant]
    assert manifest["kernel_base"] != manifest["kernel_parallel"]
    assert bench.digest(disk) == manifest["disk"]
def elapsed(row):
    t=row["ticks"]
    return {"waited":t["both_waited"], "through_drain":t["both_waited"]+t["post_wait_drain"]}
verify()
cases=[(1,1),(9,1),(1,2000000),(9,2000000)]
boots=[]
for pair in range(4):
    order=cases[pair:]+cases[:pair]
    variants=("base","parallel") if pair%2==0 else ("parallel","base")
    for variant in variants:
        verify()
        machine=bench.boot(args.output, f"p{pair+1}-{variant}",elf[variant],disk)
        try:
            owner=bench.boot_owner(machine)
            samples=[]
            for repeat in range(4):
                for policy, rounds in order:
                    out=bench.lines(machine,f"tdbench {policy} {rounds}")
                    row=bench.result(out, policy, rounds)
                    if repeat:
                        samples.append(row)
            boots.append(dict(pair=pair+1,variant=variant,owner=owner,rows=samples))
        finally:
            machine.close()
        verify()

summary=[]
for policy,iters in cases:
    for field in ("waited","through_drain"):
        medians={v:[] for v in elf}
        for run in boots:
            rows=[elapsed(x)[field] for x in run["rows"] if x["policy"]==policy and x["iterations"]==iters]
            assert len(rows)==3
            medians[run["variant"]].append(statistics.median(rows))
        ratios=[b/a for a,b in zip(medians["base"],medians["parallel"])]
        entry=dict(policy=policy,iterations=iters,metric=field,
                   baseline_ms=statistics.median(medians["base"])/10000,
                   parallel_ms=statistics.median(medians["parallel"])/10000,
                   paired_percent=100*(statistics.median(ratios)-1),
                   paired_range_percent=[100*(min(ratios)-1),100*(max(ratios)-1)])
        summary.append(entry)
report=dict(timebase_hz=10000000, receipts={k:manifest[k] for k in ("baseline","candidate","kernel_base",
            "kernel_parallel","disk","tdbench")}, boots=boots,summary=summary)
(args.output / "granularity.json").write_text(json.dumps(report,indent=2)+"\n")
print(json.dumps(summary,indent=2))

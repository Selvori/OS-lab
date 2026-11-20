"""Run 72 cold QEMU boots with all 24 orderings of policies 1, 6, 8 and 9 per block."""
import argparse
from collections import Counter
import gzip
from hashlib import sha256
from itertools import permutations
import json
from pathlib import Path
from random import Random
from statistics import median, quantiles
import sys
import tempfile

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "docs/process-lifecycle/template-bounded-reclaim/strategy-comparison"))
import collect as driver

POLICIES = (1, 6, 8, 9)
SEED = 20261121


def digest(path):
    return sha256(path.read_bytes()).hexdigest()


def sampled_orders():
    rng = Random(SEED)
    orders = []
    for _ in range(3):
        block = list(permutations(POLICIES))
        rng.shuffle(block)
        orders.extend(block)
    return orders


def measure(build, out, number, order):
    name = f"pair-{number:02d}"
    apps = [(app, build / "apps" / (app + ".bin")) for app in ("sh", "bopen")]
    boot = {"boot": number, "order": order, "warm": [], "samples": []}
    with tempfile.TemporaryDirectory(dir=out) as work:
        disk = Path(work) / "pair.img"
        driver.mkfs.make_image(disk, programs=apps)
        guest = driver.Guest(name, build / "kernel.elf", disk)
        try:
            guest.until(b"seele$ ")
            for phase, tasks in (("warm", 2), ("samples", 64)):
                for policy in order:
                    case = driver.sample(guest, f"bopen {policy} {tasks} 512", "OPEN PASS")
                    rows = case["rows"]
                    assert len(rows["OPEN"]) == 1 and rows["OPEN"][0][:3] == [policy, tasks, 512]
                    assert [r[0] for r in rows["ROUND"]] == list(range(tasks))
                    boot[phase].append(case)
        finally:
            guest.close()
        assert b"PANIC" not in guest.log and b"FAIL " not in guest.log
    boot["serial_sha256"] = digest(out / f"{name}-serial.log")
    return boot


def write_json(path, record):
    payload = (json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
    path.write_bytes(gzip.compress(payload, mtime=0))


def collect(build, reference, out):
    out.mkdir(parents=True, exist_ok=False)
    with gzip.open(reference, "rt") as source:
        ref = json.load(source)
    assert ref["harts"] == 2 and ref["memory_mib"] == 128 and ref["timebase_hz"] == 10_000_000
    hashes = {"kernel": digest(build / "kernel.elf")}
    hashes.update({a: digest(build / "apps" / f"{a}.bin") for a in ("sh", "bopen")})
    assert hashes == ref["modes"]["time"]["hashes"], (hashes, ref["modes"]["time"]["hashes"])
    group = {"hashes": hashes, "boots": []}
    record = {"commit": ref["commit"], "source_clean": ref["source_clean"], "qemu": ref["qemu"],
              "harts": 2, "memory_mib": 128, "timebase_hz": ref["timebase_hz"],
              "policies": POLICIES, "modes": {"time": group}, "order_seed": SEED}
    driver.qemu.OUT = out
    orders = sampled_orders()
    assert set(Counter(orders).values()) == {3} and len(Counter(orders)) == 24
    for i, order in enumerate(orders):
        group["boots"].append(measure(build, out, i, order))
        if (i + 1) % 24 == 0:
            write_json(out / "raw.json.gz", record)
            print(f"completed {i + 1} of 72 boots", flush=True)
    fragments = []
    for i, boot in enumerate(group["boots"]):
        name = f"pair-{i:02d}-serial.log"
        assert digest(out / name) == boot["serial_sha256"]
        fragments.extend([f"\n--- {name} ---\n".encode(), (out / name).read_bytes()])
    (out / "serial.log.gz").write_bytes(gzip.compress(b"".join(fragments), mtime=0))
    return record


def paired_interval(values):
    rng = Random(SEED)
    draws = [median(rng.choices(values, k=len(values))) for _ in range(20_000)]
    limits = quantiles(draws, n=40, method="inclusive")
    return {"paired_median_pct": median(values), "ci95_pct": [limits[0], limits[-1]]}


def summarize(record):
    series = {p: [] for p in POLICIES}
    hz = record["timebase_hz"]
    boots = record["modes"]["time"]["boots"]
    assert len(boots) == 72
    for boot in boots:
        assert sorted(boot["order"]) == list(POLICIES)
        for case in boot["samples"]:
            p, n, stride, setup, spawn, close, drain, total = case["rows"]["OPEN"][0][:8]
            per_us = 1e6 / hz
            assert n == 64 and stride == 512 and total >= close + drain + setup + spawn
            series[p].append({"spawn_us": per_us * spawn / n, "preclose_us": per_us * (total-close-drain) / n,
                              "close_us": per_us * close, "drain_us": per_us * drain,
                              "total_us": per_us * total / n})
    medians = {p: {k: median(row[k] for row in vals) for k in vals[0]} for p, vals in series.items()}
    paired = {}
    for p, base in ((6, 1), (8, 6), (9, 6), (9, 8), (9, 1)):
        paired[f"{p}/{base}"] = {}
        for metric in ("total_us", "drain_us"):
            ratios = [100 * (a[metric] / b[metric] - 1) for a, b in zip(series[p], series[base])]
            paired[f"{p}/{base}"][metric] = paired_interval(ratios)
    return {"boots": len(boots), "formal_tasks": len(boots) * 64 * len(POLICIES),
            "medians": medians, "paired": paired}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--summarize", action="store_true")
    args = parser.parse_args()
    if args.summarize:
        with gzip.open(args.out / "raw.json.gz", "rt") as raw:
            record = json.load(raw)
    else:
        record = collect(args.build.resolve(), args.reference.resolve(), args.out.resolve())
    summary = summarize(record)
    print(json.dumps(summary, indent=2))
    if not args.summarize:
        (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

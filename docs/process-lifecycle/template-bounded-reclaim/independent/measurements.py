#!/usr/bin/env python3
"""Recompute paired timings and verify the complete per-frame and per-batch observations."""
import argparse
from collections import defaultdict
import gzip
import json
from pathlib import Path
from random import Random
from statistics import median, quantiles

def read_json(path):
    op = gzip.open if path.suffix == '.gz' else open
    with op(path, 'rt', encoding='utf-8') as f:
        return json.load(f)

def one(results, name):
    values = results[name]
    assert len(values) == 1, name
    return values[0]

def audit(times, counts):
    hz = times['timebase_hz']
    assert counts['timebase_hz'] == hz
    assert times['harts'] == counts['harts'] == 2
    assert times['guest_memory_MiB'] == counts['guest_memory_MiB'] == 128
    assert not times['failures'] and not counts['failures']
    assert times['source_clean'] and counts['source_clean']
    assert times['commit'] == counts['commit']
    assert all(times['hashes'][k] == counts['hashes'][k] for k in ('bopen','bclose','sh'))
    boots = times['boots']
    assert len(boots) == len({b['boot'] for b in boots}) == 12
    primary = defaultdict(list)
    latencies = defaultdict(list)
    deltas = []
    preclose_deltas = []
    spawn_deltas = []
    for boot in boots:
        batch = {}
        for record in boot['opened']:
            row = one(record['rows'], 'OPEN')
            policy, n, stride, setup, spawn, close, drain, elapsed = row[:8]
            if (n, stride) != (64, 512):
                continue
            assert policy not in batch
            values = dict(total_us=elapsed*1e6/(n*hz), spawn_us=spawn*1e6/(n*hz),
                          setup_us=setup*1e6/hz, close_us=close*1e6/hz,
                          drain_us=drain*1e6/hz)
            batch[policy] = (elapsed, elapsed-close-drain, spawn)
            primary[policy].append(values)
        assert set(batch)=={0,1,3,6}
        for index,series in enumerate((deltas,preclose_deltas,spawn_deltas)):
            series.append(100*(batch[6][index]/batch[1][index]-1))
        by_policy = set()
        for record in boot['closed']:
            rows = record['rows']
            p, idle, start, closed, quiet, end = one(rows, 'CLOSE')[:6]
            assert (p,idle) not in by_policy and start <= closed <= quiet <= end
            by_policy.add((p,idle))
            events = rows['FRAME']
            assert len(events) == len({e[1] for e in events}) == 2048
            assert set(e[0] for e in events) == set(range(2048))
            assert all(closed<=e[2]<=e[3]<=e[4]<=quiet for e in events)
            batches = rows.get('BATCH',[])
            assert all(e[2]<=32 and e[3]<=8 and e[5]<=1 for e in batches)
            if p in (4,5,6):
                assert sum(e[3] for e in batches) == 2048
            stores = dict(rows['STORE'])
            assert len(stores)==2048
            side = [r[1] for r in rows.get('PROGRESS',[])]
            assert all(b>a for a,b in zip(side,side[1:]))
            gaps = [b-a for a,b in zip(side,side[1:]) if a<=quiet and b>=start]
            coverage = bool(side) and side[0]<=start and side[-1]>=quiet
            assert (not side) if idle else coverage
            def us(t): return t*1e6/hz
            numbers = dict(close_us=us(closed-start), close_to_all_return_us=us(max(e[4] for e in events)-closed),
                           max_borrow_to_return_us=us(max(e[4]-e[2] for e in events)),
                           max_batch_us=us(max((e[1] for e in batches),default=0)),
                           max_last_of_four_large_windows_us=us(max(stores[k] for k in (508,1020,1532,2044))),
                           progress_max_gap_us=us(max(gaps)) if gaps else None,
                           progress_covers_interval=(None if idle else coverage))
            latencies[f"{p}-{'idle' if idle else 'busy'}"].append(numbers)
        assert by_policy=={(policy,quiet) for policy in (2,4,5,6) for quiet in (0,1)}

    count_detail={}
    for record in counts['boots'][0]['opened']:
        rs=record['rows']
        p,n,stride=one(rs,'OPEN')[:3]
        if (n,stride)==(64,512):
            retains,locks,drops,drop_locks=one(rs,'REF')[:4]
            count_detail[p]=dict(retains=retains,retain_locks=locks,drops=drops,drop_locks=drop_locks)
    assert set(count_detail)=={0,1,3,6}
    assert count_detail[0]['retains']==count_detail[3]['retains']>0
    assert count_detail[3]['retain_locks']<count_detail[0]['retain_locks']
    assert count_detail[6]['retains']==count_detail[1]['retains']==0
    def summarize(by_policy):
        result={}
        for p,samples in by_policy.items():
            values={}
            for k in samples[0]:
                v=[x[k] for x in samples if x[k] is not None]
                if k=='progress_covers_interval':
                    values[k]=dict(covered=sum(v),boots=len(v)) if v else None
                else:
                    values[k]=dict(median=median(v),max=max(v)) if v else None
            result[p]=values
        return result
    def paired(values):
        rng=Random(20261120)
        medians=[median(rng.choices(values,k=len(values))) for _ in range(20000)]
        edges=quantiles(medians,n=40,method='inclusive')
        return dict(median=median(values),ci95=[edges[0],edges[-1]])
    sorted_delta=sorted(deltas)
    return dict(source=times['commit'],boot_pairs=len(boots),open=summarize(primary),
                paired_relative_total_percent=deltas,paired_median_percent=median(deltas),
                paired_bootstrap_percent={k:paired(v) for k,v in (('amortized',deltas),('preclose',preclose_deltas),('spawn',spawn_deltas))},
                paired_order_ci_percent=[sorted_delta[2],sorted_delta[-3]],
                paired_order_ci_coverage_percent=100*(1-2*79/4096),
                close=summarize(latencies),open_counts=count_detail)

if __name__=='__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('time',type=Path)
    ap.add_argument('counts',type=Path)
    ap.add_argument('output',type=Path)
    args=ap.parse_args()
    result=audit(read_json(args.time),read_json(args.counts))
    args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('pairs',result['boot_pairs'],'median %',result['paired_median_percent'],'interval %',result['paired_order_ci_percent'])

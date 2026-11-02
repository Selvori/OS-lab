#!/usr/bin/env python3
"""Lab 5 QEMU checks; --extended temporarily instruments sources, restoring bytes in finally."""
from pathlib import Path
import argparse, hashlib, json, re
import test_lab4 as base
ROOT = base.ROOT
OUT = ROOT / 'build/lab5-checks'
OUT.mkdir(parents=True, exist_ok=True)
base.OUT = OUT

def normal(label):
    result = base.normal(label)
    log = (OUT / (label + '-serial.log')).read_bytes()
    assert b'HEAP PASS: zero, split, merge, reuse, errors, 256 cycles' in log
    assert log.count(b'HEAP cross-page write OK') == 1
    result['heap_cycles'] = 256
    return result

PROBE = r"""
extern long lab5_fail_after;
extern struct proc *lab5_test_first(void);
static struct proc dummy[N_MMAP];
static bool live[USER_HEAP_PAGES];
static uint64 random_state = 0x15926;
static unsigned rnd(void)
{
    random_state = random_state * 6364136223846793005UL + 1;
    return (unsigned)(random_state >> 32);
}
static void check_model(struct proc *p)
{
    mmap_region_t *node = p->mmap;
    uintptr_t last_end = 0;
    unsigned nodes = 0;
    for (mmap_region_t *n = node; n; n = n->next) {
        assert(++nodes <= N_MMAP && n->npages && n->begin >= USER_HEAP_START &&
               n->begin + n->npages * PAGE_SIZE <= USER_HEAP_END &&
               (!last_end || last_end < n->begin), "model intervals");
        last_end = n->begin + n->npages * PAGE_SIZE;
    }
    for (unsigned i = 0; i < USER_HEAP_PAGES; ++i) {
        uintptr_t va = USER_HEAP_START + i * PAGE_SIZE, pa = 0;
        uint64 flags = 0;
        while (node && node->begin + node->npages * PAGE_SIZE <= va) node = node->next;
        bool hole = node && node->begin <= va;
        assert(hole == !live[i], "model free list mismatch");
        assert(sv39_lookup(p->pgtbl, va, &pa, &flags) == live[i], "model mapping mismatch");
        if (live[i]) {
            assert((flags & (PTE_R|PTE_W|PTE_X|PTE_U)) == (PTE_R|PTE_W|PTE_U), "heap permissions");
            assert(*(uint64 *)pa == (0xabcdef0000000000UL | i), "heap alias corruption");
            assert(*(uint64 *)(pa + PAGE_SIZE - 8) == ~((uint64)i), "heap end corruption");
        }
    }
    assert(mmap_pool_free_count() == N_MMAP - nodes, "model node leak");
}
static void mark_pages(struct proc *p, unsigned start, unsigned count)
{
    for (unsigned i = start; i < start + count; ++i) {
        uintptr_t pa;
        assert(sv39_lookup(p->pgtbl, USER_HEAP_START + i * PAGE_SIZE, &pa, NULL), "model map");
        for (size_t j = 0; j < PAGE_SIZE; ++j) assert(((uint8 *)pa)[j] == 0, "page not zero");
        *(uint64 *)pa = 0xabcdef0000000000UL | i;
        *(uint64 *)(pa + PAGE_SIZE - 8) = ~((uint64)i);
        live[i] = true;
    }
}
static void lab5_probe(void)
{
    uint64 k0 = page_free_count(PAGE_ZONE_KERNEL), u0 = page_free_count(PAGE_ZONE_USER);
    /* No metadata: process creation must unwind all already allocated frames/tables. */
    for (unsigned i = 0; i < N_MMAP; ++i) {
        dummy[i].owner = arch_cpu_id();
        assert(user_heap_init(&dummy[i]) == 0, "reserve global nodes");
    }
    assert(mmap_pool_free_count() == 0 && proc_make_first() == -1, "init node exhaustion");
    assert(page_free_count(PAGE_ZONE_KERNEL) == k0 && page_free_count(PAGE_ZONE_USER) == u0,
           "init node failure leak");
    for (unsigned i = 0; i < N_MMAP; ++i) user_heap_destroy(&dummy[i]);
    assert(mmap_pool_free_count() == N_MMAP, "init nodes restore");
    assert(proc_make_first() == 0, "probe process create");
    struct proc *p = lab5_test_first();
    uint64 k = page_free_count(PAGE_ZONE_KERNEL), u = page_free_count(PAGE_ZONE_USER);
    /* Force failure at each frame/table allocation in two three-page requests.
     * The second starts just before a 2 MiB boundary. List edits stay unpublished. */
    uintptr_t hints[] = {0x400000, 0x1ff000};
    unsigned failures = 0;
    for (unsigned h = 0; h < 2; ++h) {
        bool success = false;
        for (int f = 0; f < 16; ++f) {
            lab5_fail_after = f;
            long rc = user_heap_alloc(p, hints[h], 3);
            lab5_fail_after = -1;
            if (rc >= 0) {
                assert(rc == (long)hints[h] && user_heap_free(p, rc, 3) == 0, "injected success");
                success = true;
            } else { assert(rc == -12, "allocation failure errno"); ++failures; }
            check_model(p);
            assert(page_free_count(PAGE_ZONE_KERNEL) == k && page_free_count(PAGE_ZONE_USER) == u,
                   "heap failed allocation leak");
            if (success) break;
        }
        assert(success, "injection never completed");
    }
    /* Full arena removes the last region node; freeing all recreates it. */
    assert(user_heap_alloc(p, 0, USER_HEAP_PAGES) == USER_HEAP_START, "whole arena alloc");
    assert(p->mmap == NULL && mmap_pool_free_count() == N_MMAP, "whole node release");
    assert(user_heap_alloc(p, 0, 1) == -12, "full arena accepted");
    assert(user_heap_free(p, USER_HEAP_START, USER_HEAP_PAGES) == 0, "whole arena free");
    check_model(p);
    assert(page_free_count(PAGE_ZONE_KERNEL) == k && page_free_count(PAGE_ZONE_USER) == u,
           "whole arena page leak");
    /* Every merge branch, including isolated insert at the head and tail. */
    assert(user_heap_alloc(p, 0, USER_HEAP_PAGES) == USER_HEAP_START, "branch setup");
    assert(user_heap_free(p, USER_HEAP_START, 1) == 0, "isolated head");
    assert(user_heap_free(p, USER_HEAP_START + 3*PAGE_SIZE, 1) == 0, "isolated tail");
    assert(user_heap_free(p, USER_HEAP_START + 2*PAGE_SIZE, 1) == 0, "right merge");
    assert(user_heap_free(p, USER_HEAP_START + PAGE_SIZE, 1) == 0, "bridge merge");
    assert(user_heap_free(p, USER_HEAP_START + 4*PAGE_SIZE, USER_HEAP_PAGES-4) == 0, "left merge");
    check_model(p);
    /* Exhaust the shared pool with other PCB heads. Neither split nor isolated
     * free may change the real process; merging needs no additional node. */
    assert(user_heap_alloc(p, USER_HEAP_START + 4*PAGE_SIZE, 3) >= 0, "pool setup");
    unsigned held = (unsigned)mmap_pool_free_count();
    for (unsigned i=0; i<held; ++i) assert(user_heap_init(&dummy[i]) == 0, "hold node");
    uint64 k1=page_free_count(PAGE_ZONE_KERNEL), u1=page_free_count(PAGE_ZONE_USER);
    assert(user_heap_alloc(p, USER_HEAP_START + PAGE_SIZE, 1) == -12, "split node exhaustion");
    assert(user_heap_free(p, USER_HEAP_START + 5*PAGE_SIZE, 1) == -12, "free node exhaustion");
    assert(page_free_count(PAGE_ZONE_KERNEL)==k1 && page_free_count(PAGE_ZONE_USER)==u1,
           "node failure changed frames");
    uintptr_t pa;
    assert(sv39_lookup(p->pgtbl,USER_HEAP_START+5*PAGE_SIZE,&pa,NULL), "free failure unmapped");
    assert(user_heap_free(p,USER_HEAP_START+4*PAGE_SIZE,3)==0, "merge under exhaustion");
    for (unsigned i=0;i<held;++i) user_heap_destroy(&dummy[i]);
    check_model(p);
    /* Random independent occupancy model over 64 pages checks fragmentation,
     * exact first-fit, invalid partial ranges, contents, zeroing and NX. */
    for (unsigned step=0; step<1024; ++step) {
        unsigned start=rnd()%64, count=1+rnd()%8;
        bool allocate=(rnd()&1), fixed=(rnd()&1);
        if (allocate) {
            uintptr_t hint=fixed ? USER_HEAP_START+start*PAGE_SIZE : 0;
            if (!fixed) {
                start=0;
                for (;;++start) {
                    unsigned j=0;
                    while(j<count && !live[start+j]) ++j;
                    if(j==count) break;
                }
            }
            bool free=true;
            for(unsigned j=0;j<count;++j) if(live[start+j]) free=false;
            long rc=user_heap_alloc(p,hint,count);
            if(free) {
                assert(rc==(long)(USER_HEAP_START+start*PAGE_SIZE),"model first fit");
                mark_pages(p,start,count);
            } else assert(rc==-12,"model collision");
        } else {
            bool mapped=true;
            for(unsigned j=0;j<count;++j) if(!live[start+j]) mapped=false;
            int rc=user_heap_free(p,USER_HEAP_START+start*PAGE_SIZE,count);
            assert(rc==(mapped?0:-22),"model free result");
            if(mapped) for(unsigned j=0;j<count;++j) live[start+j]=false;
        }
        check_model(p);
    }
    /* vm_unmappages must prevalidate an entire range before changing its prefix. */
    assert(user_heap_alloc(p,0x800000,1)==0x800000,"unmap setup");
    assert(vm_unmappages(p->pgtbl,0x800000,2)==-1 &&
           sv39_lookup(p->pgtbl,0x800000,NULL,NULL),"partial unmap mutation");
    assert(user_heap_free(p,0x800000,1)==0,"unmap cleanup");
    assert(user_heap_alloc(p,0,0)==-22 && user_heap_alloc(p,1,1)==-22 &&
           user_heap_alloc(p,0,(size_t)-1)==-22 && user_heap_alloc(p,USER_HEAP_END,1)==-22 &&
           user_heap_free(p,USER_BASE,1)==-22 && user_heap_free(p,USER_HEAP_START,(size_t)-1)==-22,
           "range validation");
    proc_destroy();
    assert(page_free_count(PAGE_ZONE_KERNEL)==k0 && page_free_count(PAGE_ZONE_USER)==u0 &&
           mmap_pool_free_count()==N_MMAP,"live heap exit leak");
    for (unsigned i=0;i<32;++i) {
        assert(proc_make_first()==0,"repeat create");p=lab5_test_first();
        assert(user_heap_alloc(p,0,8)>=0 && user_heap_alloc(p,0x400000,3)>=0,"repeat alloc");
        proc_destroy();
        assert(page_free_count(PAGE_ZONE_KERNEL)==k0 && page_free_count(PAGE_ZONE_USER)==u0 &&
               mmap_pool_free_count()==N_MMAP,"repeated heap leak");
    }
    printf("LAB5 PROBE PASS: failure-points=%d random=1024 whole=4096 lifecycle=32 nodes=128\n",failures);
}
"""

def extended():
    paths=['kernel/main.c','kernel/proc/proc.c','kernel/mm/page.c','user/init.S']
    saved={n:(ROOT/n).read_bytes() for n in paths}
    hashes={n:hashlib.sha256(data).hexdigest() for n,data in saved.items()}
    results={}
    try:
        page=saved['kernel/mm/page.c'].decode().replace('void *page_alloc(page_zone_type_t type)',
             'long lab5_fail_after = -1;\nvoid *page_alloc(page_zone_type_t type)')
        page=page.replace('    if (type >= PAGE_ZONE_COUNT)',
             '    if (lab5_fail_after == 0) return NULL;\n    if (lab5_fail_after > 0) lab5_fail_after--;\n    if (type >= PAGE_ZONE_COUNT)',1)
        (ROOT/'kernel/mm/page.c').write_text(page)
        (ROOT/'kernel/proc/proc.c').write_bytes(saved['kernel/proc/proc.c']+
             b'\nstruct proc *lab5_test_first(void) { return &first; }\n')
        main=saved['kernel/main.c'].decode().replace('void main(void)',PROBE+'\nvoid main(void)')
        main=main.replace('        if (proc_make_first() != 0)',
             '        lab5_probe();\n        if (proc_make_first() != 0)')
        (ROOT/'kernel/main.c').write_text(main)
        base.build('heap-probe')
        m=base.Machine('heap-probe')
        try:
            m.until(b'LAB5 PROBE PASS:',timeout=90)
            m.until(b'process resources reclaimed; kernel remains alive\r\n')
            assert b'HEAP PASS' in m.log and b'USER PASS' in m.log and b'PANIC' not in m.log
            m.echo(b'heap-probe-recovered')
        finally: m.close()
        match=re.search(rb'failure-points=(\d+) random=1024 whole=4096 lifecycle=32 nodes=128',m.log)
        assert match
        results['probe']=dict(failure_points=int(match[1]),model_steps=1024,arena_pages=4096,
                              lifecycles=32,pool_nodes=128)
        for name in paths: (ROOT/name).write_bytes(saved[name])
        # Real user references prepopulate TLB before unmap; subsequent load/store
        # must fault, as must instruction fetch from a mapped RWU heap page.
        setup='li a0,0\nli a1,1\nli a7,5\necall\nmv s0,a0\nsd zero,0(s0)\n'
        remove='mv a0,s0\nli a1,1\nli a7,6\necall\n'
        for label,code,cause in [
            ('heap-after-free-load',setup+remove+'ld t0,0(s0)',13),
            ('heap-after-free-store',setup+remove+'sd zero,0(s0)',15),
            ('heap-nx',setup+'li t0,0x0000006f\nsw t0,0(s0)\nfence.i\njr s0',12)]:
            results[label]=base.fault(label,code,cause)
            print(label,'PASS',flush=True)
        for name in paths: (ROOT/name).write_bytes(saved[name])
        # Retain all Lab 4 protection, process-allocation and owner-UART coverage.
        results['lab4']=base.extended()
    finally:
        for name,data in saved.items(): (ROOT/name).write_bytes(data)
        assert all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==v for n,v in hashes.items())
        (OUT/'restored-sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
        base.build('restored')
    return results

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extended',action='store_true')
    args=parser.parse_args()
    base.build('normal')
    results={'normal':normal('normal')}
    print('normal',results['normal'],flush=True)
    if args.extended: results['extended']=extended()
    results['final']=normal('final')
    (OUT/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2),flush=True)

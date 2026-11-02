#!/usr/bin/env python3
"""Linux/QEMU acceptance. --extended temporarily edits sources, always restores them."""
from pathlib import Path
import argparse, hashlib, json, os, re, select, signal, subprocess, time
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/lab4-checks'
OUT.mkdir(parents=True, exist_ok=True)
CONFIG = 'CONFIG=riscv64-qemu-virt-sbi'

def build(label):
    with (OUT / (label + '-build.log')).open('wb') as log:
        subprocess.run(['make', CONFIG, 'build'], cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT, check=True)

class Machine:
    def __init__(self, label):
        self.label = label
        self.log = bytearray()
        self.pending = bytearray()
        self.p = subprocess.Popen(['make', CONFIG, 'run'], cwd=ROOT,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            bufsize=0, start_new_session=True)
    def until(self, marker, timeout=12):
        end = time.monotonic() + timeout
        while True:
            idx = self.pending.find(marker)
            if idx >= 0:
                idx += len(marker)
                result = bytes(self.pending[:idx]); del self.pending[:idx]
                return result
            if time.monotonic() > end:
                raise AssertionError(('timeout', marker, bytes(self.log[-900:])))
            if select.select([self.p.stdout], [], [], .05)[0]:
                chunk = os.read(self.p.stdout.fileno(), 65536)
                if not chunk: raise AssertionError(('QEMU stopped', self.p.poll()))
                self.log.extend(chunk); self.pending.extend(chunk)
    def send(self, payload):
        self.p.stdin.write(payload); self.p.stdin.flush()
    def echo(self, payload, expected=None):
        self.send(payload)
        expected = payload if expected is None else expected
        deadline = time.monotonic() + 10
        while len(self.pending) < len(expected):
            if time.monotonic() > deadline: raise AssertionError("echo timeout")
            if select.select([self.p.stdout], [], [], .05)[0]:
                chunk = os.read(self.p.stdout.fileno(),65536)
                if not chunk: raise AssertionError("QEMU stopped during echo")
                self.log.extend(chunk);self.pending.extend(chunk)
        received=bytes(self.pending[:len(expected)]);del self.pending[:len(expected)]
        assert received == expected, (received[:100], expected[:100])
    def close(self):
        try:
            self.send(b'\x01x')
            tail, _ = self.p.communicate(timeout=4)
            self.log.extend(tail)
            assert self.p.returncode == 0, self.p.returncode
        except BaseException:
            if self.p.poll() is None:
                os.killpg(self.p.pid, signal.SIGTERM); self.p.wait(timeout=4)
            raise
        finally:
            (OUT / (self.label + '-serial.log')).write_bytes(self.log)

def normal(label='normal', require_user_uart=False):
    m=Machine(label)
    try:
        m.until(b'USER WAIT: timer and UART active\r\n')
        # Keep each UART group unique and separate from program output.
        for i in range(16): m.echo(('USER-UART-%02d;' % i).encode())
        m.until(b'process resources reclaimed; kernel remains alive\r\n')
        result=re.search(rb'exited: status=0 syscalls=(\d+) user-timer=(\d+) user-external=(\d+)',m.log)
        assert result and b'USER PASS' in m.log and b'USER FAIL' not in m.log, bytes(m.log[-500:])
        calls,timers,external=map(int,result.groups())
        assert calls >= 4096 and timers >= 50
        if require_user_uart: assert external > 0, external
        assert m.log.count(b'UART external interrupt route ready') == 2
        assert b'PANIC' not in m.log
        m.echo(b'A')
        for i in range(64): m.echo(('batch-%02d:0123456789abcdef;' % i).encode())
        burst=(b'0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'*67)[:4096]
        m.echo(burst); m.echo(b'\x00\x7f\x80\xff')
        m.echo(b'\r',b'\r\n');m.echo(b'\n',b'\r\n')
        time.sleep(.3);m.echo(b'after-idle')
        return dict(syscalls=calls,user_timers=timers,user_external=external,
                    uart_after_exit_bytes=5777,exit_status=0)
    finally: m.close()

def fault(label, code, cause):
    (ROOT/'user/init.S').write_text('.section .text\n.globl _start\n_start:\n'+code+'\n1: j 1b\n')
    build(label)
    m=Machine(label)
    try:
        m.until(b'process resources reclaimed; kernel remains alive\r\n')
        expected=('scause=0x%016x'%cause).encode()
        assert b'USER FAULT' in m.log and expected in m.log and b'faulted: status=-1' in m.log, bytes(m.log[-700:])
        assert b'PANIC' not in m.log
        m.echo(b'fault-recovered')
    finally: m.close()
    return dict(cause=cause,recovered=True)

# A temporary C probe checks failure at every allocator call, including walk's
# intermediate tables, and verifies layout/copyin on a successful creation.
PROBE = r"""
extern long lab4_fail_after;
extern struct proc *lab4_test_first(void);
static void lab4_allocation_probe(void)
{
    uint64 k = page_free_count(PAGE_ZONE_KERNEL), u = page_free_count(PAGE_ZONE_USER);
    int success = -1;
    for (int n = 0; n < 32; ++n) {
        lab4_fail_after = n;
        int rc = proc_make_first();
        lab4_fail_after = -1;
        if (rc == 0) {
            struct proc *p = lab4_test_first();
            uintptr_t pa; uint64 flags;
            assert(!sv39_lookup(p->pgtbl, 0, NULL, NULL), "null mapped");
            assert(!sv39_lookup(p->pgtbl, DRAM_BASE, NULL, NULL), "DRAM exposed");
            assert(!sv39_lookup(p->pgtbl, UART_BASE, NULL, NULL), "MMIO exposed");
            assert(!sv39_lookup(p->pgtbl, USER_KSTACK-PAGE_SIZE, NULL, NULL), "guard mapped");
            uintptr_t vas[] = { USER_BASE, USER_BASE+PAGE_SIZE, USER_STACK,
                                USER_KSTACK, TRAPFRAME, TRAMPOLINE };
            uint64 perms[] = { PTE_R|PTE_X|PTE_U, PTE_R|PTE_W|PTE_U, PTE_R|PTE_W|PTE_U,
                               PTE_R|PTE_W, PTE_R|PTE_W, PTE_R|PTE_X };
            for (unsigned j = 0; j < 6; ++j) {
                assert(sv39_lookup(p->pgtbl, vas[j], &pa, &flags), "missing user leaf");
                assert((flags & (PTE_R|PTE_W|PTE_X|PTE_U)) == perms[j], "permissions");
            }
            unsigned char b[8];
            for (unsigned j=0;j<8;j++) b[j]=0x55;
            assert(user_copyin(p->pgtbl,b,TRAPFRAME-4,8)==-1,"cross tf accepted");
            for (unsigned j=0;j<8;j++) assert(b[j]==0x55,"partial copy");
            assert(user_copyin(p->pgtbl,b,VA_MAX-4,8)==-1,"overflow accepted");
            assert(user_copyin(p->pgtbl,b,USER_BASE+PAGE_SIZE-4,8)==0,"valid cross-page copy");
            proc_destroy(); success=n;
        }
        assert(page_free_count(PAGE_ZONE_KERNEL)==k && page_free_count(PAGE_ZONE_USER)==u,
               "allocation rollback leak");
        if (rc==0) break;
    }
    assert(success>=6,"failure injection did not reach page tables");
    for (int n=0;n<32;n++) {
        assert(proc_make_first()==0,"repeat create failed"); proc_destroy();
        assert(page_free_count(PAGE_ZONE_KERNEL)==k && page_free_count(PAGE_ZONE_USER)==u,
               "repeated destroy leak");
    }
    printf("ALLOCATION PASS: failure points=%d repeated=32 layout/copyin passed\n",success);
}
"""

def extended():
    paths=['user/init.S','kernel/main.c','kernel/mm/page.c','kernel/proc/proc.c']
    saved={n:(ROOT/n).read_bytes() for n in paths}
    hashes={n:hashlib.sha256(data).hexdigest() for n,data in saved.items()}
    results={}
    try:
        main=saved['kernel/main.c'].decode()
        (ROOT/'kernel/main.c').write_text(main.replace('    plic_inithart();',
            '    if (arch_is_boot_cpu()) plic_inithart();'))
        build('owner-uart');results['owner_uart']=normal('owner-uart',True)
        (ROOT/'kernel/main.c').write_bytes(saved['kernel/main.c'])
        cases=[('null-read','li t0, 0\nld t1, 0(t0)',13),
               ('kernel-read','li t0, 0x80200000\nld t1, 0(t0)',13),
               ('trapframe-read','li t0, 0x3fffffe000\nld t1, 0(t0)',13),
               ('kstack-read','li t0, 0x3fffffc000\nld t1, 0(t0)',13),
               ('trampoline-read','li t0, 0x3ffffff000\nld t1, 0(t0)',13),
               ('text-write','li t0, 0x1000\nsd zero, 0(t0)',15),
               ('stack-execute','addi t0, sp, -16\njr t0',12),
               ('stack-overflow','li t0, 0x3fffffcff8\nsd zero, 0(t0)',15),
               ('supervisor-csr','csrr t0, sepc',2),
               ('illegal-instruction','.word 0',2)]
        for label,code,cause in cases:
            results[label]=fault(label,code,cause)
            print(label,'PASS',flush=True)
        (ROOT/'user/init.S').write_bytes(saved['user/init.S'])
        page=saved['kernel/mm/page.c'].decode().replace('void *page_alloc(page_zone_type_t type)',
            'long lab4_fail_after = -1;\nvoid *page_alloc(page_zone_type_t type)')
        page=page.replace('    if (type >= PAGE_ZONE_COUNT)',
            '    if (lab4_fail_after == 0) return NULL;\n    if (lab4_fail_after > 0) lab4_fail_after--;\n    if (type >= PAGE_ZONE_COUNT)',1)
        (ROOT/'kernel/mm/page.c').write_text(page)
        (ROOT/'kernel/proc/proc.c').write_bytes(saved['kernel/proc/proc.c']+
            b'\nstruct proc *lab4_test_first(void) { return &first; }\n')
        main=main.replace('void main(void)',PROBE+'\nvoid main(void)')
        main=main.replace('        if (proc_make_first() != 0)',
                          '        lab4_allocation_probe();\n        if (proc_make_first() != 0)')
        (ROOT/'kernel/main.c').write_text(main)
        build('allocation');results['allocation']=normal('allocation')
        log=(OUT/'allocation-serial.log').read_bytes()
        match=re.search(rb'ALLOCATION PASS: failure points=(\d+) repeated=32',log)
        assert match,log[-900:]
        results['allocation']['failed_points']=int(match[1])
    finally:
        for name,data in saved.items(): (ROOT/name).write_bytes(data)
        assert all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==v for n,v in hashes.items())
        (OUT/'restored-sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
        build('restored')
    return results

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extended',action='store_true')
    args=parser.parse_args()
    build('normal')
    results={'normal':normal()}
    print('normal',results['normal'],flush=True)
    if args.extended: results['extended']=extended()
    results['final']=normal('final')
    (OUT/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2),flush=True)

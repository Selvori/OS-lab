#!/usr/bin/env python3
"""Lab6 Linux/QEMU acceptance. Extended probes restore exact source bytes in finally.
The Lab4/5 runners retain historical single-process expectations; use this runner.
"""
from pathlib import Path
import argparse, hashlib, json, re, time
import test_lab4 as base
ROOT=base.ROOT
OUT=ROOT/'build/lab6-checks'
OUT.mkdir(parents=True,exist_ok=True)
base.OUT=OUT
build=base.build
Machine=base.Machine


def uart(m):
    m.echo(b'A')
    for i in range(64): m.echo(('batch-%02d:0123456789abcdef;'%i).encode())
    m.echo((b'0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'*67)[:4096])
    m.echo(b'\x00\x7f\x80\xff');m.echo(b'\r',b'\r\n');m.echo(b'\n',b'\r\n')
    time.sleep(.3);m.echo(b'after-idle')


def normal(label='normal',owner_uart=False):
    m=Machine(label)
    try:
        m.until(b'USER WAIT: timer and UART active\r\n')
        for i in range(16): m.echo(('USER-UART-%02d;'%i).encode())
        m.until(b'CPU child: begin loop without syscalls\r\n')
        # Program markers may arrive during this send; consume in output order.
        m.send(b'CPU-UART-ACTIVE;')
        m.until(b'LAB6 PASS: init pid 0 remains alive\r\n',timeout=25)
        log=bytes(m.log)
        markers=[b'HEAP PASS',b'USER PASS',b'FORK/WAIT PASS',b'SLEEP PASS',
                 b'CPU child: begin',b'PREEMPT PASS',b'CPU child: registers',b'LIFECYCLE PASS',b'LAB6 PASS']
        assert all(x in log for x in markers), log[-2000:]
        assert log.index(markers[4]) < log.index(markers[5]) < log.index(markers[6])
        assert b'CPU-UART-ACTIVE;' in log
        assert b'USER FAIL' not in log and b'PANIC' not in log
        assert log.count(b'UART external interrupt route ready')==2
        uart(m)
        return dict(init_pid=0,heap_cycles=256,register_roundtrips=4096,
                    fork_wait_cycles=32,cpu_iterations=200000000,
                    parent_progress_before_cpu_exit=True,uart_bytes=5777,
                    owner_only_uart=owner_uart)
    finally: m.close()

HEADER=r"""
#include <kernel/syscall.h>
.section .text
.globl _start
.option norelax
.macro say text
 li a0,1
 la a1,\text
 la a2,\text\()_end
 sub a2,a2,a1
 li a7,SYS_WRITE
 ecall
.endm
.macro check reg,val
 li t6,\val
 bne \reg,t6,fail
.endm
_start:
"""
# Assembly above uses one backslash in GAS macros.
FOOTER=r"""
pass:
 say ok
idle:
 li a0,100
 li a7,SYS_SLEEP
 ecall
 j idle
fail:
 say bad
 li a0,1
 li a7,SYS_EXIT
 ecall
 j idle
ok:.ascii "SCENARIO PASS\n"
ok_end:
bad:.ascii "SCENARIO FAIL\n"
bad_end:
"""


def scenario(label,body,expect=None):
    (ROOT/'user/init.S').write_text(HEADER+body+'\nj pass\n'+FOOTER)
    build(label)
    m=Machine(label)
    try:
        m.until(b'SCENARIO PASS\r\n',timeout=20)
        assert b'SCENARIO FAIL' not in m.log and b'PANIC' not in m.log,bytes(m.log[-1500:])
        if expect: assert expect in m.log,bytes(m.log[-1500:])
        uart(m)
        return dict(passed=True,uart_bytes=5777)
    finally:m.close()

PROBE=r"""
extern long lab6_fail_after;
static struct proc lab6_holders[N_MMAP];
static void lab6_compare(struct proc *a, struct proc *b)
{
    assert(a->code!=b->code && a->data!=b->data && a->ustack!=b->ustack &&
           a->tf!=b->tf && a->stack_page!=b->stack_page && a->kstack!=b->kstack,
           "fork aliases fixed resources");
    void *src[]={a->code,a->data,a->ustack};
    void *dst[]={b->code,b->data,b->ustack};
    for(unsigned j=0;j<3;j++) for(unsigned k=0;k<PAGE_SIZE;k++)
        assert(((uint8*)src[j])[k]==((uint8*)dst[j])[k],"fixed copy data");
    const mmap_region_t *x=a->mmap,*y=b->mmap;
    while(x && y){
        assert(x!=y && x->begin==y->begin && x->npages==y->npages,"node copy");
        x=x->next;y=y->next;
    }
    assert(!x && !y,"list length copy");
    for(uintptr_t va=USER_HEAP_START;va<USER_HEAP_END;va+=PAGE_SIZE){
        uintptr_t pa=0,pb=0;uint64 fa=0,fb=0;
        bool ma=sv39_lookup(a->pgtbl,va,&pa,&fa),mb=sv39_lookup(b->pgtbl,va,&pb,&fb);
        assert(ma==mb,"heap occupancy copy");
        if(!ma)continue;
        assert(pa!=pb && fa==fb,"heap alias/flags");
        for(unsigned k=0;k<PAGE_SIZE;k++)assert(((uint8*)pa)[k]==((uint8*)pb)[k],"heap bytes");
    }
    for(unsigned i=0;i<NPROC;i++){
        uintptr_t va=USER_KSTACK-2*PAGE_SIZE*i;
        assert(!sv39_lookup(kernel_vm_root(),va-PAGE_SIZE,NULL,NULL),"stack guard mapped");
        if(&procs[i]!=b)assert(!sv39_lookup(b->pgtbl,va,NULL,NULL),"foreign kstack exposed");
    }
}
static void lab6_probe(void)
{
    struct proc *a=initproc;
    current[scheduler_owner]=a;a->state=PROC_RUNNING;
    assert(user_heap_alloc(a,0x3000,3)==0x3000,"parent heap");
    assert(user_heap_alloc(a,0x1ff000,3)==0x1ff000,"cross table heap");
    assert(user_heap_free(a,0x4000,1)==0,"parent hole");
    for(uintptr_t va=USER_HEAP_START;va<USER_HEAP_END;va+=PAGE_SIZE){
        uintptr_t pa;if(!sv39_lookup(a->pgtbl,va,&pa,NULL))continue;
        for(unsigned k=0;k<PAGE_SIZE;k++)((uint8*)pa)[k]=(uint8)(k+va/PAGE_SIZE);
    }
    for(unsigned k=0;k<PAGE_SIZE;k++){((uint8*)a->data)[k]=(uint8)k;((uint8*)a->ustack)[k]=(uint8)(k^0x5a);}
    uint64 k=page_free_count(PAGE_ZONE_KERNEL),u=page_free_count(PAGE_ZONE_USER);
    size_t nodes=mmap_pool_free_count();
    int points=-1;
    for(int n=0;n<128;n++){
        lab6_fail_after=n;
        long pid=proc_fork();
        lab6_fail_after=-1;
        if(pid>=0){
            struct proc *b=NULL;
            for(unsigned i=0;i<NPROC;i++)if(procs[i].state==PROC_RUNNABLE && procs[i].pid==pid)b=&procs[i];
            assert(b && b->tf->x[10]==0 && b->parent==a,"child return/parent");
            lab6_compare(a,b);
            assert(user_heap_free(b,0x3000,1)==0,"child list mutation");
            assert(sv39_lookup(a->pgtbl,0x3000,NULL,NULL),"parent lost mapping");
            proc_destroy(b);points=n;
        }else assert(pid==-12,"failure errno");
        assert(page_free_count(PAGE_ZONE_KERNEL)==k && page_free_count(PAGE_ZONE_USER)==u &&
               mmap_pool_free_count()==nodes,"fork rollback leak");
        for(unsigned i=1;i<NPROC;i++)assert(procs[i].state==PROC_UNUSED,"unpublished child leak");
        if(pid>=0)break;
    }
    assert(points>10,"failure injection too short");
    unsigned holders=0;
    while(mmap_pool_free_count()){
        struct proc *h=&lab6_holders[holders++];h->owner=scheduler_owner;
        assert(user_heap_init(h)==0,"holder init");
    }
    assert(proc_fork()==-12,"node exhausted fork accepted");
    assert(page_free_count(PAGE_ZONE_KERNEL)==k && page_free_count(PAGE_ZONE_USER)==u,
           "node failure page leak");
    user_heap_destroy(&lab6_holders[--holders]);
    assert(mmap_pool_free_count()==1,"one node expected");
    assert(proc_fork()==-12 && mmap_pool_free_count()==1,"partial node copy rollback");
    assert(page_free_count(PAGE_ZONE_KERNEL)==k && page_free_count(PAGE_ZONE_USER)==u,
           "partial node failure pages");
    for(unsigned i=0;i<holders;i++)user_heap_destroy(&lab6_holders[i]);
    assert(mmap_pool_free_count()==nodes,"node failure node leak");
    for(unsigned i=1;i<NPROC;i++)assert(proc_fork()>0,"fill table");
    assert(proc_fork()==-11,"table overflow");
    for(unsigned i=1;i<NPROC;i++)proc_destroy(&procs[i]);
    assert(page_free_count(PAGE_ZONE_KERNEL)==k && page_free_count(PAGE_ZONE_USER)==u &&
           mmap_pool_free_count()==nodes,"table lifecycle leak");
    uint8 before[4]={1,2,3,4},status[4]={9,8,7,6},after[4];
    assert(user_copyout(a->pgtbl,0x2ffe,before,4)==0,"valid copyout");
    assert(user_copyin(a->pgtbl,after,0x2ffe,4)==0,"valid copyin");
    for(unsigned i=0;i<4;i++)assert(after[i]==before[i],"cross copyout data");
    uint8 prefix[4];
    assert(user_copyin(a->pgtbl,prefix,0x3ffc,4)==0,"prefix snapshot");
    assert(user_copyout(a->pgtbl,0x3ffe,status,4)==-1,"copyout crossed hole");
    assert(user_copyin(a->pgtbl,after,0x3ffc,4)==0,"valid prefix");
    for(unsigned i=0;i<4;i++)assert(after[i]==prefix[i],"copyout partial write");
    assert(!user_writable(a->pgtbl,USER_BASE,4) && !user_writable(a->pgtbl,TRAPFRAME,4) &&
           !user_writable(a->pgtbl,VA_MAX-2,4) && !user_writable(a->pgtbl,(uintptr_t)-2,4),
           "copyout permissions/range");
    /* Restore the actual production init's pristine bytes before dispatch. */
    user_heap_destroy(a);assert(user_heap_init(a)==0,"reset parent heap");
    memset(a->data,0,PAGE_SIZE);memset(a->ustack,0,PAGE_SIZE);
    a->state=PROC_RUNNABLE;current[scheduler_owner]=NULL;next_pid=1;
    printf("FORK PROBE PASS: allocation points=%d slots=16 nodes=128 deep-copy/copyout/rollback\\n",points);
}
"""


PROBE=PROBE.replace(chr(92)*2,chr(92))

def extended():
    names=['user/init.S','kernel/main.c','kernel/mm/page.c','kernel/proc/proc.c']
    saved={n:(ROOT/n).read_bytes() for n in names}
    results={}
    try:
        # All IRQ delivery on owner: user preemption and scheduler idle still work.
        main=saved['kernel/main.c'].decode()
        (ROOT/'kernel/main.c').write_text(main.replace('    plic_inithart();','    if (arch_is_boot_cpu()) plic_inithart();'))
        build('owner-uart');results['owner_uart']=normal('owner-uart',True)
        (ROOT/'kernel/main.c').write_bytes(saved['kernel/main.c'])
        # Child exits before wait; invalid pointer must not consume its zombie.
        results['early-exit']=scenario('early-exit',r"""
 li a7,SYS_FORK
 ecall
 bltz a0,fail
 beqz a0,child
 mv s0,a0
 li a0,5
 li a7,SYS_SLEEP
 ecall
 li a0,0x1000
 li a7,SYS_WAIT
 ecall
 check a0,-14
 li a0,0x2000
 li a7,SYS_WAIT
 ecall
 bne a0,s0,fail
 li t0,0x2000
 lw t1,0(t0)
 check t1,-123
 li a0,0
 li a7,SYS_WAIT
 ecall
 check a0,-10
 j pass
child:
 li a0,-123
 li a7,SYS_EXIT
 ecall
 j fail
""")
        # Both orphan adoption paths: a live grandchild and an already zombie.
        for early in [False,True]:
            delay='li a0,5\nli a7,SYS_SLEEP\necall\n' if early else ''
            childdelay='' if early else 'li a0,8\nli a7,SYS_SLEEP\necall\n'
            results['orphan-'+str(early)]=scenario('orphan-'+str(early),r"""
 li a7,SYS_FORK
 ecall
 bltz a0,fail
 beqz a0,parent
 li s0,0
 li s1,2
reap:
 li a0,0x2000
 li a7,SYS_WAIT
 ecall
 bltz a0,fail
 li t0,0x2000
 lw t1,0(t0)
 add s0,s0,t1
 addi s1,s1,-1
 bnez s1,reap
 check s0,33
 li a0,0
 li a7,SYS_WAIT
 ecall
 check a0,-10
 j pass
parent:
 li a7,SYS_FORK
 ecall
 bltz a0,fail
 beqz a0,grandchild
"""+delay+r"""
 li a0,11
 li a7,SYS_EXIT
 ecall
 j fail
grandchild:
"""+childdelay+r"""
 li a0,22
 li a7,SYS_EXIT
 ecall
 j fail
""")
        # Two unconditional forks yield 4 processes, 3 descendants; each reaps its own children.
        results['fork-tree']=scenario('fork-tree',r"""
 li s0,0
 li a7,SYS_FORK
 ecall
 bltz a0,fail
 snez t0,a0
 add s0,s0,t0
 li a7,SYS_FORK
 ecall
 bltz a0,fail
 beqz a0,tree_leaf
 addi s0,s0,1
 j tree_wait
tree_leaf:
 li s0,0
tree_wait:
 beqz s0,tree_exit
 li a0,0
 li a7,SYS_WAIT
 ecall
 bltz a0,fail
 addi s0,s0,-1
 j tree_wait
tree_exit:
 li a7,SYS_GETPID
 ecall
 beqz a0,pass
 li a0,0
 li a7,SYS_EXIT
 ecall
 j fail
""")
        # Staggered sleepers; wait status can straddle two writable user pages.
        results['sleep-boundary']=scenario('sleep-boundary',r"""
 li a0,0
 li a1,2
 li a7,SYS_MMAP
 ecall
 check a0,0x3000
 li s0,3
spawn:
 li a7,SYS_FORK
 ecall
 bltz a0,fail
 beqz a0,sleeper
 addi s0,s0,-1
 bnez s0,spawn
 li s0,3
 li s1,0
sleep_reap:
 li a0,0x3ffe
 li a7,SYS_WAIT
 ecall
 bltz a0,fail
 li t0,0x3ffe
 lbu t1,0(t0)
 add s1,s1,t1
 addi s0,s0,-1
 bnez s0,sleep_reap
 check s1,6
 li a0,0
 li a7,SYS_SLEEP
 ecall
 check a0,0
 li a7,SYS_YIELD
 ecall
 check a0,0
 j pass
sleeper:
 li a7,SYS_TICKS
 ecall
 mv s1,a0
 slli a0,s0,1
 li a7,SYS_SLEEP
 ecall
 bnez a0,fail
 li a7,SYS_TICKS
 ecall
 sub t0,a0,s1
 slli t1,s0,1
 bltu t0,t1,fail
 mv a0,s0
 li a7,SYS_EXIT
 ecall
 j fail
""")
        # Slot exhaustion including zombies, followed by wait/reuse.
        results['slots']=scenario('slots',r"""
 li s0,15
fill:
 li a7,SYS_FORK
 ecall
 bltz a0,fail
 beqz a0,slot_child
 addi s0,s0,-1
 bnez s0,fill
 li a7,SYS_FORK
 ecall
 check a0,-11
 li s0,15
slot_reap:
 li a0,0
 li a7,SYS_WAIT
 ecall
 bltz a0,fail
 addi s0,s0,-1
 bnez s0,slot_reap
 li a7,SYS_FORK
 ecall
 bltz a0,fail
 beqz a0,slot_child
 li a0,0
 li a7,SYS_WAIT
 ecall
 bltz a0,fail
 j pass
slot_child:
 li a0,0
 li a7,SYS_EXIT
 ecall
 j fail
""")
        # Two CPU-only children retain every non-counter integer register.
        proc_source=saved['kernel/proc/proc.c'].decode()
        (ROOT/'kernel/proc/proc.c').write_text(proc_source.replace(
            '    p->status = status;',
            '    if(status==77){ assert(p->user_timers>=2 && p->switches>=3,"no asynchronous preemption"); printf("ASYNC STATS pid=%d timers=%d switches=%d\\n",p->pid,(int)p->user_timers,(int)p->switches); }\n    p->status = status;'))
        body='\n li s0,2\nasync_spawn:\n li a7,SYS_FORK\n ecall\n bltz a0,fail\n beqz a0,async_child\n addi s0,s0,-1\n bnez s0,async_spawn\n li s0,2\nasync_wait:\n li a0,0x2000\n li a7,SYS_WAIT\n ecall\n bltz a0,fail\n li t0,0x2000\n lw t1,0(t0)\n check t1,77\n addi s0,s0,-1\n bnez s0,async_wait\n j pass\nasync_child:\n addi sp,sp,-16\n li t0,0x5555\n sd t0,0(sp)\n'
        regs=['ra','gp','tp','t2','s0','s1','a0','a1','a2','a3','a4','a5','a6','a7',
              's2','s3','s4','s5','s6','s7','s8','s9','s10','s11','t3','t4','t5','t6']
        for i,r in enumerate(regs):body+=' li %s,%d\n'%(r,0x100+i)
        body+=' li t0,100000000\n li t1,0\nasync_loop:\n addi t1,t1,7\n addi t0,t0,-1\n bnez t0,async_loop\n'
        body+=' check t6,%d\n'%(0x100+len(regs)-1)
        for i,r in enumerate(regs[:-1]):body+=' check %s,%d\n'%(r,0x100+i)
        body+=' check t0,0\n check t1,700000000\n ld t0,0(sp)\n check t0,0x5555\n li a0,77\n li a7,SYS_EXIT\n ecall\n j fail\n'
        results['async-registers']=scenario('async-registers',body,b'ASYNC STATS')
        (ROOT/'kernel/proc/proc.c').write_bytes(saved['kernel/proc/proc.c'])
        # Faulting descendants become zombies and are reaped by a live init.
        faults=[('null','ld t0,0(zero)',13),('text','li t0,0x1000\nsd zero,0(t0)',15),
                ('heap-nx','li a0,0\nli a1,1\nli a7,SYS_MMAP\necall\njr a0',12),
                ('after-unmap','li a0,0\nli a1,1\nli a7,SYS_MMAP\necall\nmv s2,a0\nli a1,1\nli a7,SYS_MUNMAP\necall\nld t0,0(s2)',13),
                ('supervisor','csrr t0,sepc',2)]
        for name,code,cause in faults:
            results['fault-'+name]=scenario('fault-'+name,r"""
 li a7,SYS_FORK
 ecall
 bltz a0,fail
 beqz a0,fault_child
 mv s0,a0
 li a0,0x2000
 li a7,SYS_WAIT
 ecall
 bne a0,s0,fail
 li t0,0x2000
 lw t1,0(t0)
 check t1,-1
 j pass
fault_child:
"""+code,('scause=0x%016x'%cause).encode())
        (ROOT/'user/init.S').write_bytes(saved['user/init.S'])
        # Failure injection and independent resource/copy checks before first dispatch.
        page=saved['kernel/mm/page.c'].decode().replace('void *page_alloc(page_zone_type_t type)',
            'long lab6_fail_after=-1;\nvoid *page_alloc(page_zone_type_t type)')
        page=page.replace('    if (type >= PAGE_ZONE_COUNT)',
            '    if(lab6_fail_after==0)return NULL;\n    if(lab6_fail_after>0)--lab6_fail_after;\n    if (type >= PAGE_ZONE_COUNT)',1)
        (ROOT/'kernel/mm/page.c').write_text(page)
        proc=saved['kernel/proc/proc.c'].decode()
        proc=proc.replace('void proc_scheduler(void)','static void lab6_probe(void);\nvoid proc_scheduler(void)')
        proc=proc.replace('    assert(initproc && !proc_current(), "scheduler init");',
                          '    assert(initproc && !proc_current(), "scheduler init");\n    lab6_probe();')
        proc=proc.replace('static struct proc procs[NPROC];',
            'static uint64 lab6_k,lab6_u; static size_t lab6_n;\nstatic struct proc procs[NPROC];')
        proc=proc.replace('    struct proc *child = proc_allocate();',
            '    lab6_k=page_free_count(PAGE_ZONE_KERNEL); lab6_u=page_free_count(PAGE_ZONE_USER); lab6_n=mmap_pool_free_count();\n    struct proc *child = proc_allocate();')
        proc=proc.replace('            proc_destroy(child);',
            '            proc_destroy(child);\n            assert(page_free_count(PAGE_ZONE_KERNEL)==lab6_k && page_free_count(PAGE_ZONE_USER)==lab6_u && mmap_pool_free_count()==lab6_n,"real wait leak");')
        (ROOT/'kernel/proc/proc.c').write_text(proc+PROBE)
        build('probe');results['probe']=normal('probe')
        log=(OUT/'probe-serial.log').read_bytes()
        match=re.search(rb'FORK PROBE PASS: allocation points=(\d+)',log)
        assert match,log[-2000:]
        results['probe']['allocation_failure_points']=int(match[1])
    finally:
        for n,data in saved.items():(ROOT/n).write_bytes(data)
        hashes={n:hashlib.sha256(v).hexdigest() for n,v in saved.items()}
        assert all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==h for n,h in hashes.items())
        (OUT/'restored-sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
        build('restored')
    return results

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--extended',action='store_true');args=ap.parse_args()
    build('normal');results={'normal':normal()};print('normal PASS',flush=True)
    if args.extended:results['extended']=extended()
    results['final']=normal('final')
    (OUT/('results-extended.json' if args.extended else 'results.json')).write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2),flush=True)

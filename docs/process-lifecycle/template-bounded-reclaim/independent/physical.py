#!/usr/bin/env python3
"""Check original physical frames in paused QEMU guests using only kernel ELF symbols and RAM."""
import argparse
from collections import Counter
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / 'tools'))
import mkfs

DRAM = 0x80000000
DRAM_BYTES = 128 << 20
PAGE = 4096
HEAP = 0x3000
EXPRESSIONS = {
    'zone_start': '&zones[1].start', 'zone_end': '&zones[1].end',
    'free_head': '&zones[1].freelist', 'free_count': '&zones[1].free_count',
    'refs': '&user_refs', 'procs': '&procs', 'stride': 'sizeof(struct proc)',
    'slots': 'sizeof(procs)/sizeof(procs[0])',
    'pid_offset': '&((struct proc *)0)->pid', 'state_offset': '&((struct proc *)0)->state',
    'root_offset': '&((struct proc *)0)->pgtbl',
}

def symbols(elf):
    cmd = ['gdb-multiarch', '-nx', '-batch', str(elf)]
    for expr in EXPRESSIONS.values():
        cmd.extend(['-ex', f'p/x (unsigned long)({expr})'])
    data = subprocess.check_output(cmd, text=True)
    values = [int(x, 16) for x in re.findall(r'^\$\d+ = (0x[0-9a-f]+)\s*$', data, re.M)]
    assert len(values) == len(EXPRESSIONS), data
    return dict(zip(EXPRESSIONS, values))

def filehash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def send(command, wire, **arguments):
    wire.write((json.dumps(dict(execute=command, arguments=arguments))+'\n').encode())
    wire.flush()
    while True:
        reply = json.loads(wire.readline())
        if 'error' in reply:
            raise ValueError((command,reply))
        if 'return' in reply:
            return reply['return']

class Serial:
    def __init__(self, proc):
        self.proc = proc
        self.log = bytearray()
        self.consumed = 0

    def until(self, token, timeout=50):
        end = time.monotonic() + timeout
        while True:
            at = self.log.find(token, self.consumed)
            if at >= 0:
                self.consumed = at + len(token)
                return
            recent = self.log[self.consumed:]
            if any(x in recent for x in (b'AUDIT FAIL line', b'PANIC', b'USER FAULT')):
                raise ValueError(recent[-1500:])
            if self.proc.poll() is not None or time.monotonic() > end:
                raise RuntimeError((token, self.proc.poll(), self.log[-1000:]))
            if select.select([self.proc.stdout], [], [], .1)[0]:
                self.log.extend(os.read(self.proc.stdout.fileno(), 65536))

    def write(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

class RAM:
    def __init__(self, memory, sym):
        self.mem = memory
        self.sym = sym
        self.start = self.u64(sym['zone_start'])
        self.end = self.u64(sym['zone_end'])
        head = self.u64(sym['free_head'])
        self.free = set()
        while head:
            assert self.start <= head < self.end and not head % PAGE and head not in self.free, hex(head)
            self.free.add(head)
            head = self.u64(head)
        assert len(self.free) == self.u64(sym['free_count'])

    def number(self, pa, fmt):
        assert DRAM <= pa <= DRAM + DRAM_BYTES - struct.calcsize(fmt), hex(pa)
        return struct.unpack_from(fmt, self.mem, pa - DRAM)[0]

    def u64(self, pa):
        return self.number(pa, '<Q')

    def process(self, pid):
        g = self.sym
        for k in range(g['slots']):
            slot = g['procs'] + g['stride'] * k
            if self.number(slot+g['state_offset'], '<I') and self.number(slot+g['pid_offset'], '<i') == pid:
                return self.u64(slot + g['root_offset'])
        return None

    def mapping(self, root, va):
        for level in (2, 1, 0):
            entry = self.u64(root + 8 * ((va >> (12 + 9 * level)) & 511))
            if not entry & 1:
                return None
            root = (entry >> 10) << 12
            if entry & 14:
                assert level == 0 and entry & 16 and entry & 2, (level, hex(va), hex(entry))
                return root
        raise ValueError('missing leaf')

    def refs(self, frame):
        assert self.start <= frame < self.end and not frame % PAGE
        return self.number(self.sym['refs'] + 2 * ((frame - self.start) // PAGE), '<H')

def run(args, phase_dir):
    sym = symbols(args.elf)
    serial = None
    info = dict(commit=subprocess.check_output(['git','rev-parse',args.revision],text=True).strip(),
                case=args.case,policy=args.policy,granularity=args.granularity,elf_sha256=filehash(args.elf),
                program_sha256=filehash(args.guest),snapshots=[])
    original = []
    pages = 2048 if args.case == 'pin' else 1
    real_pins = {0,509,1021,1533,2045}
    branches = []
    with tempfile.TemporaryDirectory(prefix='seele-paudit-') as temp:
        temp = Path(temp)
        disk = temp/'disk.img'
        mkfs.make_image(disk,programs=[('sh',args.shell),('audit',args.guest)])
        qmp = temp/'mon.sock'
        proc = subprocess.Popen(['qemu-system-riscv64','-machine','virt','-bios','default',
            '-m','128M','-smp','2','-display','none','-serial','stdio','-monitor','none',
            '-qmp',f'unix:{qmp},server,nowait','-global','virtio-mmio.force-legacy=false',
            '-drive',f'file={disk},if=none,format=raw,id=seele-disk','-device',
            'virtio-blk-device,drive=seele-disk','-kernel',str(args.elf)],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        serial = Serial(proc)
        try:
            serial.until(b'seele$ ')
            with socket.socket(socket.AF_UNIX) as monitor:
                monitor.settimeout(20)
                monitor.connect(str(qmp))
                with monitor.makefile('rwb') as wire:
                    assert 'QMP' in json.loads(wire.readline())
                    send('qmp_capabilities',wire)
                    serial.write(f'audit {args.policy} {args.case}\r'.encode())
                    phases=['ORIGIN','ATTACHED','HELD']
                    if args.case=='owner': phases.append('CREATOR RELEASED')
                    for phase in [*phases,'FINAL']:
                        serial.until(f'AUDIT {phase}\r\n'.encode())
                        parent=int(re.search(rb'AUDIT ORIGIN PID (\d+)',serial.log)[1])
                        if phase=='ATTACHED':
                            family=re.search(rb'AUDIT FAMILY (\d+) (\d+) (\d+)',serial.log)
                            assert int(family[1])==parent
                            branches=[int(family[2]),int(family[3])]
                        send('stop',wire)
                        assert send('query-status',wire)['running'] is False
                        dump=temp/'ram.bin'
                        send('pmemsave',wire,val=DRAM,size=DRAM_BYTES,filename=str(dump))
                        with dump.open('rb') as f, mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as mem:
                            ram=RAM(mem,sym)
                            root=ram.process(parent)
                            assert root
                            parent_pages=[ram.mapping(root,HEAP+i*PAGE) for i in range(pages)]
                            if phase=='ORIGIN':
                                original.extend(parent_pages)
                                assert None not in original and len(set(original))==pages
                                info['original_frames_sha256']=hashlib.sha256(
                                    b''.join(struct.pack('<Q',pa) for pa in original)).hexdigest()
                            refs=[ram.refs(pa) for pa in original]
                            free=[i for i,pa in enumerate(original) if pa in ram.free]
                            row=dict(phase=phase,ref_histogram=dict(Counter(refs)),originals_on_free_list=len(free),
                                     zone_free_pages=len(ram.free),parent_mapped=sum(pa is not None for pa in parent_pages))
                            if phase in ('ORIGIN','ATTACHED') or (phase=='HELD' and args.case=='owner'):
                                assert parent_pages==original,phase
                            else:
                                assert not any(parent_pages),phase
                            if phase=='ORIGIN': want_refs=[1]*pages
                            elif phase=='ATTACHED': want_refs=[2]*pages
                            elif phase=='HELD' and args.case=='owner': want_refs=[1]
                            elif phase=='HELD' and args.granularity=='window': want_refs=[1]*pages
                            elif phase=='HELD': want_refs=[int(i in real_pins or i-1 in real_pins) for i in range(pages)]
                            else: want_refs=[0]*pages
                            info['snapshots'].append(row)
                            assert refs==want_refs, (phase,row,Counter(want_refs))
                            assert free==[i for i,n in enumerate(want_refs) if n==0],(phase,row)
                            branch_mapped=[]
                            for role,pid in enumerate(branches):
                                br=ram.process(pid)
                                if phase=='FINAL':
                                    assert br is None,(phase,pid)
                                    continue
                                assert br
                                masks=[]
                                for i,frame in enumerate(original):
                                    pa=ram.mapping(br,HEAP+i*PAGE)
                                    expected=phase=='ATTACHED' or (phase=='HELD' and args.case=='pin' and i-role in real_pins)
                                    assert pa==(frame if expected else None),(phase,role,i,pa,expected)
                                    if pa: masks.append(i)
                                branch_mapped.append(len(masks))
                                if phase=='HELD' and masks:
                                    row.setdefault('live_pins',[]).append(masks)
                            row['branch_mapped']=branch_mapped
                        dump.unlink()
                        send('cont',wire)
                        serial.write(b'g')
                    serial.until(b'INDEPENDENT AUDIT PASS\r\n')
                    serial.until(b'seele$ ')
                    assert b'PANIC' not in serial.log
                    send('quit',wire)
            proc.wait(timeout=5)
            info['passed']=True
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=5)
            phase_dir.mkdir(parents=True,exist_ok=True)
            (phase_dir/'serial.log').write_bytes(serial.log)
            (phase_dir/'physical-frames.json').write_text(json.dumps(info,indent=2)+'\n')
    print(json.dumps([(s['phase'],s['originals_on_free_list'],s['ref_histogram']) for s in info['snapshots']]))

if __name__=='__main__':
    ap=argparse.ArgumentParser()
    for name in ('elf','shell','guest','out'):
        ap.add_argument('--'+name,required=True,type=Path)
    ap.add_argument('--revision', default='HEAD', help='exact source revision built into the supplied ELF')
    ap.add_argument('--policy',required=True,type=int)
    ap.add_argument('--case',required=True,choices=('pin','owner'))
    ap.add_argument('--granularity',choices=('page','window'),required=True)
    args=ap.parse_args()
    run(args,args.out)

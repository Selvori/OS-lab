#!/usr/bin/env python3
"""Exercise Lab 7 on real virtio-blk. Uses only a disposable raw image."""
import hashlib
import json
import os
import subprocess

import test_lab4 as qemu
import test_lab6 as lab6

OUT = qemu.ROOT / 'build/lab7-checks'
OUT.mkdir(parents=True, exist_ok=True)
qemu.OUT = OUT
DISK = OUT / 'probe.img'
SECTORS = 8192

PROBE = r'''
#include <kernel/string.h>

static unsigned cache_checked;
static unsigned workers;

static void check_pattern(const uint8 *bytes, uint64 sector)
{
    for (unsigned i = 0; i < BLOCK_SIZE; ++i)
        assert(bytes[i] == (uint8)(sector * 7 + i * 13), "disk data mismatch");
}

static void lab7_probe(int cpu)
{
    uint8 bytes[BLOCK_SIZE];
    assert(block_capacity() == 8192, "disk capacity");
    memset(bytes, 0x5a, sizeof(bytes));
    assert(block_read(block_capacity(), bytes) == -22, "capacity boundary");
    assert(block_write(UINT64_MAX, bytes) == -22, "sector overflow");
    assert(block_read(0, NULL) == -22 && block_write(0, NULL) == -22,
           "null buffer");
    for (unsigned i = 0; i < BLOCK_SIZE; ++i)
        assert(bytes[i] == 0x5a, "bad read changed destination");

    if (arch_is_boot_cpu()) {
        /* Other boot-time clients may already have populated the cache. */
        enum { TEST_FIRST = 64 };
        struct block_cache_stats before = block_stats();
        for (unsigned i = 0; i < BLOCK_CACHE_SIZE; ++i)
            assert(block_read(TEST_FIRST + i, bytes) == 0, "cold read");
        struct block_cache_stats filled = block_stats();
        assert(filled.misses - before.misses == BLOCK_CACHE_SIZE, "cold fills");
        assert(block_read(TEST_FIRST, bytes) == 0, "hot read");
        assert(block_read(TEST_FIRST + BLOCK_CACHE_SIZE, bytes) == 0, "evict");
        assert(block_read(TEST_FIRST, bytes) == 0, "MRU was evicted");
        assert(block_read(TEST_FIRST + 1, bytes) == 0, "refill");
        struct block_cache_stats stats = block_stats();
        assert(stats.hits - filled.hits == 2 && stats.misses - filled.misses == 2 &&
               stats.evictions - filled.evictions == 2, "LRU counters");

        for (unsigned i = 0; i < 65540; ++i)
            assert(block_read(32 + i % (BLOCK_CACHE_SIZE + 1), bytes) == 0,
                   "virtqueue wrap read");
        printf("LAB7 LRU PASS: hits=2 misses=18 evictions=2; queue wrapped\n");
        __atomic_store_n(&cache_checked, 1, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&cache_checked, __ATOMIC_ACQUIRE) == 0) {
        }
    }
    for (unsigned i = 0; i < 64; ++i) {
        uint64 sector = block_capacity() - 1 - i * 2 - cpu;
#ifdef LAB7_RESTART
        assert(block_read(sector, bytes) == 0, "restart read");
#else
        for (unsigned j = 0; j < BLOCK_SIZE; ++j)
            bytes[j] = (uint8)(sector * 7 + j * 13);
        assert(block_write(sector, bytes) == 0, "write");
        memset(bytes, 0, sizeof(bytes));
        assert(block_read(sector, bytes) == 0, "read after write");
#endif
        check_pattern(bytes, sector);
    }
    printf("LAB7 CPU %d PASS: 64 sectors\n", cpu);
    __atomic_add_fetch(&workers, 1, __ATOMIC_ACQ_REL);
    if (arch_is_boot_cpu()) {
        while (__atomic_load_n(&workers, __ATOMIC_ACQUIRE) != NCPU) {
        }
        assert(block_flush() == 0, "flush");
        printf("LAB7 FLUSH PASS\n");
    }
}
'''


def run(label, probe, disk=True):
    original_env = os.environ.get('DISK')
    os.environ['DISK'] = str(DISK)
    try:
        if disk:
            m = qemu.Machine(label)
        else:
            class NoDisk(qemu.Machine):
                def __init__(self):
                    self.label = label
                    self.log = bytearray()
                    self.pending = bytearray()
                    self.p = subprocess.Popen([
                        'qemu-system-riscv64', '-machine', 'virt', '-smp', '2',
                        '-m', '128M', '-nographic', '-kernel',
                        'build/riscv64-qemu-virt-sbi/kernel.elf',
                    ], cwd=qemu.ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, bufsize=0, start_new_session=True)
            m = NoDisk()
        try:
            m.until(b'block: ' + (b'4' if disk else b'0') + b' MiB available\r\n')
            m.until(b'LAB6 PASS: init pid 0 remains alive\r\n', timeout=40)
            if probe:
                for marker in (b'LAB7 LRU PASS', b'LAB7 CPU 0 PASS',
                               b'LAB7 CPU 1 PASS', b'LAB7 FLUSH PASS'):
                    assert marker in m.log, bytes(m.log[-1800:])
            assert b'PANIC' not in m.log and b'FAIL:' not in m.log
            lab6.uart(m)
        finally:
            m.close()
    finally:
        if original_env is None:
            del os.environ['DISK']
        else:
            os.environ['DISK'] = original_env


def main():
    with DISK.open('wb') as image:
        image.truncate(SECTORS * 512)
    source = qemu.ROOT / 'kernel/main.c'
    original = source.read_bytes()
    marker = b'void main(void)'
    entry = b'    arch_irq_enable();'
    assert original.count(marker) == original.count(entry) == 1
    modified = original.replace(marker, PROBE.encode() + b'\n' + marker)
    modified = modified.replace(entry, entry + b'\n    lab7_probe(cpuid);')
    try:
        source.write_bytes(modified)
        run('write', probe=True)
        raw = DISK.read_bytes()
        assert raw[:(SECTORS - 128) * 512] == bytes((SECTORS - 128) * 512)
        for sector in range(SECTORS - 128, SECTORS):
            expected = bytes((sector * 7 + i * 13) & 255 for i in range(512))
            assert raw[sector * 512:(sector + 1) * 512] == expected, sector
        source.write_bytes(b'#define LAB7_RESTART\n' + modified)
        run('restart', probe=True)
        assert DISK.read_bytes() == raw
    finally:
        source.write_bytes(original)
        qemu.build('restored')
        assert source.read_bytes() == original

    run('formal', probe=False)
    run('no-disk', probe=False, disk=False)
    summary = dict(sectors=SECTORS, disk_bytes=len(raw), concurrent_harts=2,
                   host_verified_sectors=128, restart_verified=True,
                   lru=dict(hits=2, misses=18, evictions=2),
                   wrap_reads_per_probe=65540, uart_bytes_per_run=5777,
                   no_disk_boot=True, main_sha256=hashlib.sha256(original).hexdigest())
    (OUT / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()

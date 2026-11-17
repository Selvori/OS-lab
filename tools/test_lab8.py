#!/usr/bin/env python3
"""Verify inode, directory and path operations through QEMU and a raw image."""
import hashlib
import json
import os
import struct

import mkfs
import test_lab4 as qemu
import test_lab6 as lab6

OUT = qemu.ROOT / 'build/lab8-checks'
OUT.mkdir(parents=True, exist_ok=True)
qemu.OUT = OUT

PROBE = r'''
#include <kernel/string.h>
static uint8 input[FS_MAX_FILE];

#if LAB8_SCENARIO <= 2
static void check_file(uint32 ino)
{
    uint8 output[513];
    uint32 offset = 0;
    while (offset < sizeof(input)) {
        int n = fs_read(ino, offset, output, sizeof(output));
        assert(n > 0, "early EOF");
        for (int i = 0; i < n; ++i)
            assert(output[i] == input[offset + i], "file content");
        offset += n;
    }
    assert(fs_read(ino, offset, output, sizeof(output)) == 0, "EOF");
}

#endif

static void lab8_probe(void)
{
    assert(fs_lookup(FS_ROOT, "/") == FS_ROOT, "root");
    int etc = fs_lookup(FS_ROOT, "//etc/./");
    int motd = fs_lookup(etc, "../etc//motd");
    assert(etc > 0 && motd > 0, "relative or dot traversal");
    assert(fs_lookup(FS_ROOT, "/etc/motd/") == FS_NOTDIR, "trailing slash");
    assert(fs_lookup(FS_ROOT, "/etc/motd/file") == FS_NOTDIR, "nondirectory");
    assert(fs_lookup(FS_ROOT, "/unknown") == FS_NOENT, "missing");
    assert(fs_lookup(FS_ROOT, "/abcdefghijklmnopqrstuvwxyz123") == FS_NAMETOOLONG, "long name");
    char text[32];
    assert(fs_read(motd, 0, text, sizeof(text)) == 20, "seed file");
    const char *expected = "Welcome to SeeleFS.\n";
    for (unsigned i = 0; i < 20; ++i)
        assert(text[i] == expected[i], "seed bytes");
    struct fs_dirent entry;
    assert(fs_readdir(motd, 0, &entry) == FS_NOTDIR, "file readdir");
    assert(fs_readdir(FS_ROOT, 1, &entry) == FS_INVAL, "bad dir offset");

    for (unsigned i = 0; i < sizeof(input); ++i)
        input[i] = (uint8)(i * 17 + i / 513);
    input[511] = 0xff;
    input[512] = 0x00;

#if LAB8_SCENARIO == 1
    int dir = fs_create(FS_ROOT, "/test/", FS_DIR);
    assert(dir > 0, "directory create");
    int file = fs_create(dir, "./payload", FS_FILE);
    assert(file > 0, "file create");
    assert(fs_create(dir, "./payload", FS_FILE) == FS_EXIST, "duplicate");
    assert(fs_create(dir, "../missing/x", FS_FILE) == FS_NOENT, "missing parent");
    assert(fs_create(dir, ".", FS_FILE) == FS_INVAL, "dot mutation");
    assert(fs_write(file, 5, input, 1) == FS_INVAL, "sparse file");
    assert(fs_write(file, 0, input, sizeof(input)) == (int)sizeof(input), "max write");
    assert(fs_write(file, sizeof(input), input, 1) == FS_NOSPC, "file cap");
    const uint8 edit[] = { 0xff, 0x00 };
    assert(fs_write(file, 511, edit, sizeof(edit)) == 2, "cross-sector overwrite");
    check_file(file);
    assert(fs_remove(FS_ROOT, "/test") == FS_NOTEMPTY, "nonempty directory");
    assert(fs_remove(dir, "./payload/") == FS_NOTDIR, "trailing remove");
    int scratch = fs_create(dir, "scratch", FS_FILE);
    assert(scratch > 0 && fs_write(scratch, 0, input, 513) == 513, "scratch");
    assert(fs_truncate(scratch) == 0 && fs_read(scratch, 0, text, 1) == 0,
           "truncate");
    assert(fs_remove(dir, "scratch") == 0, "scratch unlink");
    assert(fs_create(dir, "reuse", FS_DIR) == scratch, "inode reuse");
    assert(fs_remove(dir, "reuse") == 0, "directory removal");
    assert(fs_lookup(dir, "reuse") == FS_NOENT, "directory gone");
    assert(fs_sync() == 0, "flush files");
#elif LAB8_SCENARIO == 2
    int file = fs_lookup(FS_ROOT, "/test/./../test/payload");
    assert(file > 0, "persisted file");
    check_file(file);
    struct fs_info stat;
    assert(fs_stat(file, &stat) == 0 && stat.size == FS_MAX_FILE && stat.type == FS_FILE,
           "persistent inode fields");
    int dir = fs_lookup(FS_ROOT, "/test");
    int next = fs_readdir(dir, 0, &entry);
    assert(next > 0 && entry.ino == (uint32)dir, "dot entry");
    next = fs_readdir(dir, next, &entry);
    assert(next > 0 && entry.ino == FS_ROOT, "dot-dot entry");
    next = fs_readdir(dir, next, &entry);
    assert(next > 0 && entry.ino == (uint32)file, "file entry");
    assert(fs_readdir(dir, next, &entry) == 0, "skip vacant slot");
#elif LAB8_SCENARIO == 3
    int file = fs_create(FS_ROOT, "/fill", FS_FILE);
    assert(file > 0, "small-disk file");
    assert(fs_write(file, 0, input, sizeof(input)) == 5120, "short write");
    assert(fs_write(file, 5120, input, 1) == FS_NOSPC, "full disk");
    assert(fs_create(FS_ROOT, "/dir", FS_DIR) == FS_NOSPC, "full mkdir");
    assert(fs_lookup(FS_ROOT, "/dir") == FS_NOENT, "partial mkdir exposed");
    assert(fs_remove(FS_ROOT, "/fill") == 0, "unlink full");
    int dir = fs_create(FS_ROOT, "/dir", FS_DIR);
    assert(dir == file, "inode and sector reuse");
    assert(fs_remove(FS_ROOT, "/dir") == 0 && fs_sync() == 0, "free all");
#elif LAB8_SCENARIO == 4
    for (unsigned i = 0; i < 124; ++i) {
        char name[] = { 'f', '0' + i / 100, '0' + (i / 10) % 10, '0' + i % 10, '\0' };
        assert(fs_create(FS_ROOT, name, FS_FILE) == (int)i + 4, "allocate inode slots");
    }
    assert(fs_create(FS_ROOT, "full", FS_FILE) == FS_NOSPC, "inode full");
    assert(fs_remove(FS_ROOT, "f071") == 0, "free inode");
    assert(fs_create(FS_ROOT, "reuse", FS_FILE) == 75, "reuse full inode table");
    assert(fs_sync() == 0, "flush inodes");
#endif
    printf("LAB8 CASE %d PASS\n", LAB8_SCENARIO);
}
'''


def boot(label, disk, mount, case=None):
    old = os.environ.get('DISK')
    os.environ['DISK'] = str(disk)
    try:
        machine = qemu.Machine(label)
        try:
            machine.until(mount)
            if case is not None:
                machine.until(f'LAB8 CASE {case} PASS\r\n'.encode(), timeout=30)
            machine.until(b'LAB6 PASS: init pid 0 remains alive\r\n', timeout=30)
            assert b'PANIC' not in machine.log and b'USER FAIL' not in machine.log
            lab6.uart(machine)
        finally:
            machine.close()
    finally:
        if old is None:
            del os.environ['DISK']
        else:
            os.environ['DISK'] = old


def verify_bitmap(path):
    """Check that every allocated sector has exactly one owner."""
    raw = path.read_bytes()
    _, _, sectors, bitmap, _, inodes, data, _ = struct.unpack_from('<8I', raw, 512)
    live = set(range(data))
    for ino in range(1, 128):
        kind, size, *pointers = struct.unpack_from('<16I', raw, inodes * 512 + ino * 64)
        blocks = pointers[:12]
        used = (size + 511) // 512
        if kind == 0:
            assert size == 0
        for address in blocks[:used]:
            assert data <= address < sectors and address not in live
            live.add(address)
        assert not any(blocks[used:])
    on_disk = {i for i in range(sectors) if raw[bitmap * 512 + i // 8] & (1 << (i % 8))}
    assert live == on_disk


def main():
    images = {label: OUT / f'{label}.img' for label in ('persistent', 'small', 'inodes', 'invalid')}
    for path in images.values():
        if path.exists():
            path.unlink()
    mkfs.make_image(images['persistent'])
    mkfs.make_image(images['small'], 32)
    mkfs.make_image(images['inodes'])
    invalid = bytearray(images['persistent'].read_bytes())
    struct.pack_into('<I', invalid, 512 + 24, 999999)
    images['invalid'].write_bytes(invalid)

    source = qemu.ROOT / 'kernel/main.c'
    original = source.read_bytes()
    start = b'void main(void)'
    hook = b'        plic_init();'
    assert original.count(start) == original.count(hook) == 1
    edited = original.replace(start, PROBE.encode() + b'\n' + start)
    edited = edited.replace(hook, b'        lab8_probe();\n' + hook)
    mounted = b'SeeleFS: mounted root inode 1\r\n'
    try:
        for case, label in enumerate(('persistent', 'persistent', 'small', 'inodes'), 1):
            source.write_bytes(f'#define LAB8_SCENARIO {case}\n'.encode() + edited)
            boot(f'case-{case}', images[label], mounted, case)
            verify_bitmap(images[label])
    finally:
        source.write_bytes(original)
        qemu.build('restored')
        assert source.read_bytes() == original

    boot('formal', images['persistent'], mounted)
    boot('invalid', images['invalid'], b'SeeleFS: unavailable (-22)\r\n')
    assert images['invalid'].read_bytes() == invalid
    result = dict(cross_sector=True, file_bytes=6144, restart=True,
                  short_write_bytes=5120, allocated_inodes=127,
                  bitmap_verified=True, malformed_image_unchanged=True,
                  uart_bytes_per_run=5777, main_sha256=hashlib.sha256(original).hexdigest())
    (OUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

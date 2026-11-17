#!/usr/bin/env python3
"""Drive the real U-mode shell and descriptor probe over the QEMU serial port."""
import json
import os
from pathlib import Path
import struct
import subprocess

import mkfs
import test_lab4 as qemu
import test_lab6 as lab6
import test_lab8 as lab8

OUT = qemu.ROOT / 'build/lab9-checks'
OUT.mkdir(parents=True, exist_ok=True)
qemu.OUT = OUT
APPS = Path('build/riscv64-qemu-virt-sbi/apps')
PROMPT = b'seele$ '


def command(machine, command, expected):
    machine.send(command + b'\r')
    output = machine.until(PROMPT)
    assert output == expected + PROMPT, (command, output, expected)
    assert b'PANIC' not in machine.log and b'USER FAULT' not in machine.log


def line(machine, command_text, result=b''):
    command(machine, command_text, command_text + b'\r\n' + result)


def contents(image, directory_name, filename):
    raw = image.read_bytes()
    inode_start = struct.unpack_from('<I', raw, 512 + 20)[0]

    def read(ino):
        kind, size, *blocks = struct.unpack_from('<16I', raw, inode_start * 512 + ino * 64)
        data = b''.join(raw[sec * 512:(sec + 1) * 512] for sec in blocks[:(size + 511) // 512])
        return kind, data[:size]

    def lookup(ino, name):
        kind, data = read(ino)
        assert kind == 2
        for number, entry in struct.iter_unpack('<I28s', data):
            if number and entry.split(b'\0', 1)[0] == name:
                return number
        raise AssertionError((ino, name))

    kind, data = read(lookup(lookup(1, directory_name), filename))
    assert kind == 1
    return data


def main():
    targets = [str(APPS / (name + '.bin')) for name in ('sh', 'echo', 'fdprobe')]
    subprocess.run(['make', qemu.CONFIG, 'build', *targets], cwd=qemu.ROOT, check=True)
    image = OUT / 'shell-test.img'
    legacy = OUT / 'legacy-fs.img'
    image.unlink(missing_ok=True)
    legacy.unlink(missing_ok=True)
    binaries = [(name, qemu.ROOT / target) for name, target in zip(('sh', 'echo', 'fdprobe'), targets)]
    mkfs.make_image(image, programs=binaries)
    mkfs.make_image(legacy)
    before = image.read_bytes()
    try:
        mkfs.make_image(image)
    except FileExistsError:
        pass
    else:
        raise AssertionError("mkfs overwrote an existing image")
    assert image.read_bytes() == before

    old = os.environ.get('DISK')
    os.environ['DISK'] = str(image)
    try:
        machine = qemu.Machine('commands')
        try:
            machine.until(PROMPT)
            assert b'init: starting /bin/sh' in machine.log
            line(machine, b'pwd', b'/\r\n')
            line(machine, b'ls', b'.\r\n..\r\netc\r\nbin\r\n')
            line(machine, b'cat /etc/motd', b'Welcome to SeeleFS.\r\n')
            command(machine, b'pwx\x7fd', b'pwx\b \bd\r\n/\r\n')
            line(machine, b'echo "two words" xyz', b'two words xyz\r\n')
            line(machine, b'/bin/echo \'a b\' empty', b'a b empty\r\n')
            line(machine, b'echo "unterminated', b'syntax: error -22\r\n')
            line(machine, b'no-such-command', b'no-such-command: error -2\r\n')
            line(machine, b'/etc/motd', b'/etc/motd: error -8\r\n')
            command(machine, b'x' * 260, b'x' * 255 + b'\r\ninput: error -36\r\n')
            line(machine, b'mkdir /work')
            line(machine, b'cd /work//.')
            line(machine, b'pwd', b'/work\r\n')
            line(machine, b'write note "hello world"')
            line(machine, b'append note again')
            line(machine, b'cat ../work/note', b'hello world\r\nagain\r\n')
            line(machine, b'fdprobe "two words"', b'FD PROBE BEGIN\r\n'
                 b'FD PASS: inheritance, dup, user ranges, limits and rollback\r\n'
                 b'FD EXEC PASS: cwd/offset retained, heap replaced\r\n')
            line(machine, b'ls', b'.\r\n..\r\nnote\r\nrecords\r\n')
            line(machine, b'cd /')
            line(machine, b'rm work', b'rm: error -39\r\n')
            line(machine, b'mkdir empty')
            line(machine, b'rm empty')
            line(machine, b'ls /empty', b'ls: error -2\r\n')
            line(machine, b'sync')
        finally:
            machine.close()

        lab8.verify_bitmap(image)
        assert contents(image, b'work', b'note') == b'hello world\nagain\n'
        chunk = bytes(ord('a') + i % 26 for i in range(256))
        assert contents(image, b'work', b'records') == chunk * 2 + chunk[:8]

        machine = qemu.Machine('restart')
        try:
            machine.until(PROMPT)
            line(machine, b'cat /work/note', b'hello world\r\nagain\r\n')
            line(machine, b'rm /work/note')
            line(machine, b'rm /work/records')
            line(machine, b'rm /work')
            line(machine, b'ls /work', b'ls: error -2\r\n')
        finally:
            machine.close()
        lab8.verify_bitmap(image)

        os.environ['DISK'] = str(legacy)
        lab6.OUT = OUT
        fallback = lab6.normal('lab6-fallback')
    finally:
        if old is None:
            del os.environ['DISK']
        else:
            os.environ['DISK'] = old

    result = dict(shell=True, disk_backed_executable=True, argv_quotes=True, descriptor_limit=16,
                  open_description_limit=48,
                  shared_offset_fork_dup=True, child_exit_releases_files=True,
                  failed_exec_preserves_memory=True, exec_inherits_files_cwd=True,
                  exec_replaces_heap=True, persisted_binary_bytes=520, bitmap_verified=True,
                  lab6_fallback=fallback)
    (OUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

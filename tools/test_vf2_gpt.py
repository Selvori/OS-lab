#!/usr/bin/env python3
"""Test the VF2 partition selector with synthetic mirrored GPT images."""
import ctypes
from pathlib import Path
import struct
import subprocess
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
SECTORS = 32768
SECTOR = 512
PARTITION_TYPE = '9ea193f1-66ec-4307-a879-474c515e4e45'
LINUX_TYPE = '0fc63daf-8483-4772-8e79-3d69d8477de4'
DISK_ID = uuid.UUID('6fd32ec2-90dd-4b32-8be7-0ddcd69eda00')
READ = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p)


def fixture(parts):
    table = bytearray(128 * 128)
    for index, (kind, first, count, name) in enumerate(parts):
        struct.pack_into('<16s16sQQQ72s', table, 128 * index,
                         uuid.UUID(kind).bytes_le, uuid.UUID(int=index + 1).bytes_le,
                         first, first + count - 1, 0, name.encode('utf-16le'))
    table_crc = zlib.crc32(table)
    blocks = {}
    for current, other, entries in ((1, SECTORS - 1, 2),
                                    (SECTORS - 1, 1, SECTORS - 33)):
        header = bytearray(SECTOR)
        struct.pack_into('<8sIIIIQQQQ16sQIII', header, 0,
                         b'EFI PART', 0x10000, 92, 0, 0, current, other,
                         34, SECTORS - 34, DISK_ID.bytes_le, entries, 128, 128, table_crc)
        struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
        blocks[current] = header
        for offset in range(32):
            blocks[entries + offset] = table[offset * SECTOR:(offset + 1) * SECTOR]
    return blocks


def main():
    out = ROOT / 'build/lab9-vf2-sd'
    out.mkdir(parents=True, exist_ok=True)
    lib = out / 'gpt-test.so'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I.', '-Iinclude',
                    '-shared', '-fPIC', 'drivers/block/gpt.c', '-o', str(lib)],
                   check=True, cwd=ROOT)
    parser = ctypes.CDLL(str(lib)).gpt_seele_partition
    parser.argtypes = (READ, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64))
    parser.restype = ctypes.c_int

    def check(table, result, selected=0):
        @READ
        def read(sector, destination):
            if sector not in table:
                return -5
            ctypes.memmove(destination, bytes(table[sector]), SECTOR)
            return 0

        value = ctypes.c_uint64()
        assert parser(read, SECTORS, ctypes.byref(value)) == result
        assert value.value == selected

    root = (LINUX_TYPE, 2048, 4096, 'Root Partition')
    target = (PARTITION_TYPE, 8192, 8192, 'seelefs')
    check(fixture([root, target]), 0, 8192)
    check(fixture([root]), -2)
    check(fixture([root, (LINUX_TYPE, 8192, 8192, 'seelefs')]), -2)
    check(fixture([root, (PARTITION_TYPE, 8192, 4096, 'seelefs')]), -22)
    check(fixture([root, (PARTITION_TYPE, 4096, 8192, 'seelefs')]), -22)
    check(fixture([root, (PARTITION_TYPE, 28672, 8192, 'seelefs')]), -22)
    check(fixture([root, target, (PARTITION_TYPE, 16384, 8192, 'seelefs')]), -22)
    for sector in (1, 2, SECTORS - 1, SECTORS - 33):
        broken = fixture([root, target])
        broken[sector][8] ^= 1
        check(broken, -22)
    print('GPT PASS: unique bounded partition; missing, overlap, duplicate, size and CRC rejection')


if __name__ == '__main__':
    main()

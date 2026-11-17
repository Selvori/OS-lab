#!/usr/bin/env python3
"""Create a new little-endian SeeleFS v1 image. Existing files are preserved."""
import argparse
from pathlib import Path
import struct

BLOCK = 512
INODES = 128
DIRECT = 12
MAGIC = 0x5345454C
FILE, DIR = 1, 2
ROOT = 1
MOTD = b'Welcome to SeeleFS.\n'


def dirent(ino, name):
    name = name.encode('ascii')
    if not 1 <= len(name) <= 27:
        raise ValueError('invalid directory name')
    return struct.pack('<I28s', ino, name)


def make_image(path, sectors=8192):
    if not 22 <= sectors <= 8192:
        raise ValueError('sector count must be between 22 and 8192')
    bitmap = 2
    bitmap_count = (sectors + 4095) // 4096
    inodes = bitmap + bitmap_count
    data = inodes + INODES * 64 // BLOCK
    image = bytearray(sectors * BLOCK)
    struct.pack_into('<8I', image, BLOCK, MAGIC, 1, sectors, bitmap,
                     bitmap_count, inodes, data, ROOT)
    next_sector = data

    def add(ino, kind, contents):
        nonlocal next_sector
        blocks = []
        for offset in range(0, len(contents), BLOCK):
            payload = contents[offset:offset + BLOCK]
            start = next_sector * BLOCK
            image[start:start + len(payload)] = payload
            blocks.append(next_sector)
            next_sector += 1
        fields = blocks + [0] * (DIRECT + 2 - len(blocks))
        struct.pack_into('<16I', image, inodes * BLOCK + ino * 64,
                         kind, len(contents), *fields)

    add(ROOT, DIR, dirent(ROOT, '.') + dirent(ROOT, '..') + dirent(2, 'etc'))
    add(2, DIR, dirent(2, '.') + dirent(ROOT, '..') + dirent(3, 'motd'))
    add(3, FILE, MOTD)
    for sec in range(next_sector):
        image[bitmap * BLOCK + sec // 8] |= 1 << (sec % 8)
    with Path(path).open('xb') as out:
        out.write(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('--sectors', type=int, default=8192)
    args = parser.parse_args()
    try:
        make_image(args.image, args.sectors)
    except (OSError, ValueError) as err:
        parser.error(str(err))


if __name__ == '__main__':
    main()

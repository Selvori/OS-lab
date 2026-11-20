#!/usr/bin/env python3
"""Build a normal kernel and exercise the four strategies in separate QEMU guests."""
import argparse
from pathlib import Path
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output_directory', type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    out = args.output_directory.resolve()
    build = out / 'build'
    apps = build / 'apps'
    rule = ('$(BUILD)/apps/paudit.elf: $(AUDIT_SOURCE) $(APP_DEPS)\n'
            '\t$(compile_user_program)\n')
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    subprocess.run([
        'make', '-s', '-j2', '-f', 'Makefile', '-f', '-', 'CONFIG=riscv64-qemu-virt-sbi',
        f'BUILD={build}', f'AUDIT_SOURCE={here / "guest.c"}', 'build',
        str(apps / 'sh.bin'), str(apps / 'paudit.bin')
    ], input=rule, text=True, check=True)
    for policy, scenario in ((2, 'pin'), (4, 'pin'), (5, 'pin'), (6, 'pin'), (6, 'owner')):
        subprocess.run([
            sys.executable, str(here / 'physical.py'),
            '--elf', str(build / 'kernel.elf'), '--shell', str(apps / 'sh.bin'),
            '--guest', str(apps / 'paudit.bin'), '--revision', revision,
            '--policy', str(policy), '--case', scenario,
            '--granularity', 'page' if policy == 6 else 'window',
            '--out', str(out / f'signed-p{policy}-{scenario}')
        ], check=True)

if __name__ == '__main__':
    main()

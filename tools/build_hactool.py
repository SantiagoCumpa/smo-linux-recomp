"""Rebuild the hactool binary the repository carries: tools/bin/hactool (maintainers only; players use it as it is).

    python tools/build_hactool.py --src <checkout of SciresM/hactool> [--out tools/bin]
    python tools/build_hactool.py --fetch [--out tools/bin]

hactool decrypts the game's NSP files (tools/extract/nsp.py). The repository keeps the binary, not hactool's sources, so
a fresh clone needs neither network nor a C compiler for it. This script makes that binary: SciresM/hactool at the
pinned commit below (the one totk-pc-recomp uses), reset, with tools/hactool/*.patch applied, built with the system's cc
and make and linked statically (it then runs on any x86-64 Linux, whatever its glibc), stripped. hactool carries its own
mbedtls. --fetch clones the commit from GitHub into build/tools/hactool-src; --src takes a checkout of it.
hactool's LICENSE goes beside the binary; THIRD_PARTY_NOTICES.md records where it comes from.

tools/hactool/bktr-new-layout.patch: the 1.4.1 update's RomFS patch (BKTR) has tables after the subsection table and
its encrypted data ends at the relocation table; unpatched hactool stops with "Invalid BKTR layout!".
"""
import argparse
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

URL = 'https://github.com/SciresM/hactool.git'
COMMIT = '1d64a83450e025622f3468c28fc4164dad2c5ef6'
PATCH_DIR = os.path.join(REPO, 'tools', 'hactool')
BIN = os.path.join(REPO, 'tools', 'bin', 'hactool')      # the binary the setup uses
CONFIG_MK = ('CC = cc\nCFLAGS = -O2 -Wall -Wextra -pedantic -std=gnu11 -fPIC\n'
             'LDFLAGS = -lmbedtls -lmbedx509 -lmbedcrypto -static -s\n')


def run(cmd, cwd=None):
    print('  $ ' + ' '.join(cmd))
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('build_hactool: %s failed:\n%s' % (cmd[0], (r.stdout + r.stderr)[-3000:]))
    return r.stdout


def build(src, out_dir):
    for tool in ('git', 'cc', 'make', 'patch'):
        if not shutil.which(tool):
            sys.exit('build_hactool: %s is missing (Arch: sudo pacman -S base-devel git)' % tool)
    run(['git', 'checkout', '--quiet', '--force', COMMIT], cwd=src)
    run(['git', 'clean', '-xdfq'], cwd=src)
    for name in sorted(os.listdir(PATCH_DIR)):
        if name.endswith('.patch'):
            run(['patch', '-p1', '-i', os.path.join(PATCH_DIR, name)], cwd=src)
    with open(os.path.join(src, 'config.mk'), 'w') as f:
        f.write(CONFIG_MK)
    run(['make', '-j%d' % (os.cpu_count() or 4)], cwd=src)
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'hactool')
    shutil.copy2(os.path.join(src, 'hactool'), exe)
    os.chmod(exe, 0o755)
    shutil.copy2(os.path.join(src, 'LICENSE'), os.path.join(out_dir, 'hactool.LICENSE'))
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--src', help='a checkout of SciresM/hactool (reset to the pinned commit)')
    g.add_argument('--fetch', action='store_true', help='clone SciresM/hactool from GitHub into build/tools/hactool-src')
    ap.add_argument('--out', default=os.path.dirname(BIN), help='where the binary goes (default: tools/bin)')
    a = ap.parse_args()
    src = a.src
    if a.fetch:
        src = T.build('tools', 'hactool-src')
        if not os.path.isdir(os.path.join(src, '.git')):
            if os.path.exists(src):
                shutil.rmtree(src)
            os.makedirs(os.path.dirname(src), exist_ok=True)
            run(['git', 'clone', '--quiet', URL, src])
    print('hactool: ' + build(os.path.abspath(src), os.path.abspath(a.out)))


if __name__ == '__main__':
    main()

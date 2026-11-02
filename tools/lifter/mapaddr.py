"""Name the host function at an offset of an executable or shared library (Linux, ELF), from its symbol table.

    python tools/lifter/mapaddr.py <smo_native | libsmo_game.so | their .map> <offset hex> [<offset hex> ...]

Offsets are what a crash report prints ("smo_native+0x...": relative to the module's load base, from dladdr). The
symbols are read with `nm` (build_exe.py links with -g1, unstripped); a .map path names the binary beside it.
"""
import os
import subprocess
import sys


def load(path):
    """(base, [(address, name, '')...] sorted). base is 0: ELF symbol values of a PIE/shared object are offsets."""
    if path.endswith('.map'):
        path = path[:-4]
    syms = []
    if os.path.exists(path):
        r = subprocess.run(['nm', '--defined-only', '-C', path], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            parts = line.split(' ', 2)
            if len(parts) == 3 and parts[1] in 'tTwW':
                try:
                    syms.append((int(parts[0], 16), parts[2], ''))
                except ValueError:
                    pass
    syms.sort()
    return 0, syms


def name_at(syms, addr):
    best = None
    for s in syms:
        if s[0] <= addr:
            best = s
        else:
            break
    return '%s+0x%x' % (best[1], addr - best[0]) if best else '?'


def main():
    base, syms = load(sys.argv[1])
    for off in sys.argv[2:]:
        print('0x%s -> %s' % (off, name_at(syms, base + int(off, 16))))


if __name__ == '__main__':
    main()

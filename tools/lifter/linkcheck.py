"""Link every object of the lifted main with stubs of the runtime interface only, into build/obj/libmain_linkcheck.so.

    python tools/lifter/linkcheck.py

Succeeds only if the linker resolves every symbol: each f_<addr> the lifted code calls must be defined by the
lifted code (an unresolved one is a function the lifter referenced but did not produce; ld.lld with --no-undefined).
Then loads the library and checks that fn_table has one entry per listed function. The library is a check artefact,
never run as the game.
"""
import ctypes
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402
OBJ = T.build('obj', 'main')
GEN = T.build('lift', 'main')
CLANG = T.CLANG
FUNCS = T.build('functions', 'main_functions.tsv')


def main():
    objs = sorted(os.path.join(OBJ, n) for n in os.listdir(OBJ) if n.endswith(T.OBJ_EXT))
    rsp = os.path.join(OBJ, '_linkcheck.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
    dll = T.build('obj', 'libmain_linkcheck.so')
    t = time.time()
    r = subprocess.run([CLANG, '-shared', '-fPIC', '-fuse-ld=lld', '-O1', '-w', '-I', os.path.join(REPO, 'runtime', 'core'), '-I', GEN,
                        os.path.join(HERE, 'linkcheck_stub.c'), '@' + rsp, '-Wl,--no-undefined', '-lm', '-o', dll], capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    unresolved = [l for l in out.split('\n') if 'undefined symbol' in l.lower()]
    print('link: %d objects, exit %d, %.0fs, %d unresolved-symbol lines' % (len(objs), r.returncode, time.time() - t,
                                                                            len(unresolved)))
    if r.returncode:
        print(out[-3000:])
        sys.exit(1)
    lib = ctypes.CDLL(dll)
    lib.linkcheck_count.restype = ctypes.c_int
    n = lib.linkcheck_count()
    listed = sum(1 for _ in open(FUNCS, encoding='utf-8')) - 1
    print('fn_table entries: %d   listed functions: %d   %s' % (n, listed, 'MATCH' if n == listed else 'DIFFERENT'))
    print('library size: %.0f MB' % (os.path.getsize(dll) / 2 ** 20))
    sys.exit(0 if n == listed else 1)


if __name__ == '__main__':
    main()

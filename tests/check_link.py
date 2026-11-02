"""The host runtime builds, links and starts on this system, without the dump.

    python tests/check_link.py

1. Every runtime source (tools/lifter/build_exe.py's list) is compiled with stand-ins for the headers the dump gives
   (build/gen/image.h, sdklift_install.h: written here to build/tmp/check_link/gen) and build/gen/sdktest_data.h
   (tools/lifter/gen_sdktest.py, independent of the game).
2. An empty game library (game_shim.c and empty tables, soname libsmo_game.so) and empty SDK tables stand for the lifted
   code; smo_native is linked against it and the system libraries exactly as build_exe.py does: no symbol may be left
   undefined, and the program must find its game library through its rpath ($ORIGIN).
3. The program's own checks that need no game: --shaderc-selftest (GLSL -> SPIR-V) and SMO_PAD_SELFTEST=1 (controllers
   through SDL3 virtual devices; SDL's offscreen video driver).
Exit status 0 when all of it passes. Everything is written under build/tmp/check_link.
"""
import concurrent.futures
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools', 'lifter'))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import build_exe as B  # noqa: E402
import target as T  # noqa: E402
OUT = os.path.join(REPO, 'build', 'tmp', 'check_link')
STUB_GEN = os.path.join(OUT, 'gen')

IMAGE_H = r'''/* check_link.py stub (the real one: tools/lifter/gen_tables.py from the dump) */
#ifndef SMO_IMAGE_H
#define SMO_IMAGE_H
#define GAME_VERSION "1.4.1"
#define SDK_VERSION 0x07030200u
#define GAME_ELF_DEFAULT "../dump/update/elf/main.elf"
#define GAME_BUILD_ID "0"
#define GUEST_FILE_IMAGE 0x1000ULL
#define GUEST_IMAGE_SIZE 0x1000ULL
#define GUEST_RO_END 0x1000ULL
#define GUEST_RELA 0ULL
#define GUEST_RELASZ 0ULL
#define GUEST_JMPREL 0ULL
#define GUEST_PLTRELSZ 0ULL
#define GUEST_DYNSYM 0ULL
#define GUEST_DYNSTR 0ULL
#define GUEST_INIT_ARRAY (GUEST_BASE + 0ULL)
#define GUEST_INIT_ARRAY_SIZE 0ULL
#define GUEST_TEXT_SIZE 0x1000ULL
#define GAME_SYM_malloc (GUEST_BASE + 0x10ULL)
#define GAME_SYM_free (GUEST_BASE + 0x10ULL)
#define GAME_SYM_calloc (GUEST_BASE + 0x10ULL)
#define GAME_SYM_realloc (GUEST_BASE + 0x10ULL)
#define GAME_SYM_aligned_alloc (GUEST_BASE + 0x10ULL)
#define GAME_SYM_nninitStartup (GUEST_BASE + 0x10ULL)
#define GAME_SYM_nnMain (GUEST_BASE + 0x10ULL)
#define GAME_SYM__ZdlPv (GUEST_BASE + 0x10ULL)
#define GAME_SYM__ZN4sead12PtrArrayImpl4sortEPFiPKvS2_E 0ULL
#define GAME_SYM__ZN3agl6driver6NVNMgr16registerSampler_EiPK10NVNsamplerj 0ULL
#define GAME_SYM__ZN3agl6driver6NVNMgr18compareSamplerKey_EPKNS1_10SamplerKeyES4_ 0ULL
#define GAME_VSYM_StageScene_init 0ULL
#define GAME_FN_ChangeStageInfo_ctor 0ULL
#define GAME_FN_tryChangeNextStage 0ULL
#define GAME_FN_updatePoseTrans 0ULL
#define GAME_FN_setVelocityZero 0ULL
#define SDK_IMAGE_SIZE_GEN 0x1000ULL
#define SDK_TEXT_SIZE_GEN 0x1000ULL
#endif
'''
SDKLIFT_INSTALL_H = 'static const char *const sdk_install_names[] = { 0 };\n'
GAME_STUB = """#include "runtime.h"
const FnEntry fn_table[1];
const int fn_table_count = 0;
const DynSym dyn_syms[1];
const int dyn_sym_count = 0;
"""
SDK_STUB = """#include "runtime.h"
#include "hostrt.h"
#include "sdklift.h"
const SdkData sdk_data[1];
const int sdk_data_count = 0;
const SdkFn sdk_fns[1];
const int sdk_fn_count = 0;
const char *const sdk_host_names[1];
const int sdk_host_name_count = 0;
const uint64_t sdk_ctors[1];
const int sdk_ctor_count = 0;
"""


def write(path, text):
    with open(path, 'w') as f:
        f.write(text)
    return path


def main():
    os.makedirs(STUB_GEN, exist_ok=True)
    write(os.path.join(STUB_GEN, 'image.h'), IMAGE_H)
    write(os.path.join(STUB_GEN, 'sdklift_install.h'), SDKLIFT_INSTALL_H)
    if not os.path.exists(os.path.join(B.GEN, 'sdktest_data.h')):
        subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'lifter', 'gen_sdktest.py')], check=True)
    flags = ['-O1', '-g0'] + B.COMMON + T.runtime_includes() + ['-I', STUB_GEN, '-I', B.GEN,
             '-Dfn_table=(*game_fn_table_p)', '-Dfn_table_count=(*game_fn_table_count_p)',
             '-Ddyn_syms=(*game_dyn_syms_p)', '-Ddyn_sym_count=(*game_dyn_sym_count_p)'] + B.pkg_config('--cflags')

    def cc(src):
        obj = os.path.join(OUT, os.path.basename(src)[:-2] + '.o')
        r = subprocess.run([T.CLANG, '-c', '-fPIC'] + flags + [src, '-o', obj], capture_output=True, text=True)
        return src, obj, r.returncode, r.stderr

    srcs = [T.runtime(d, n) for d, names in B.HOST_SOURCES.items() for n in names]
    srcs.append(write(os.path.join(OUT, 'sdk_stub.c'), SDK_STUB))
    objs, failed = [], 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 8) as pool:
        for src, obj, code, err in pool.map(cc, srcs):
            if code:
                failed += 1
                print('FAILED %s\n%s' % (os.path.basename(src), err[-3000:]))
            objs.append(obj)
    print('compiled: %d files, %d failed' % (len(srcs), failed))
    if failed:
        sys.exit(1)
    lib = os.path.join(OUT, T.GAME_LIB)
    r = subprocess.run([T.CLANG, '-shared', '-fPIC', '-fvisibility=hidden', '-fuse-ld=lld', '-w'] + T.runtime_includes() +
                       [T.runtime('core', 'game_shim.c'), write(os.path.join(OUT, 'game_stub.c'), GAME_STUB),
                        '-Wl,--no-undefined', '-Wl,-soname,' + T.GAME_LIB, '-lm', '-o', lib], capture_output=True, text=True)
    if r.returncode:
        sys.exit('check_link: the empty game library did not link:\n' + r.stderr[-3000:])
    exe = os.path.join(OUT, T.EXE)
    r = subprocess.run([T.CLANG, '-fuse-ld=lld'] + objs + [lib, '-Wl,-rpath,$ORIGIN', '-Wl,--error-limit=0'] +
                       B.pkg_config('--libs') + B.LIBS + ['-o', exe], capture_output=True, text=True)
    undefined = sorted({l.split('undefined symbol: ')[1] for l in r.stderr.splitlines() if 'undefined symbol: ' in l})
    if r.returncode:
        print(r.stderr[-3000:] if not undefined else 'undefined symbols (%d): %s' % (len(undefined), ' '.join(undefined)))
        sys.exit('check_link: smo_native did not link')
    print('linked %s (%.1f MB)' % (os.path.relpath(exe, REPO), os.path.getsize(exe) / 2 ** 20))
    env = dict(os.environ, XDG_CACHE_HOME=os.path.join(REPO, 'build', 'cache'), TMPDIR=os.path.join(REPO, 'build', 'tmp'),
               SDL_VIDEO_DRIVER='offscreen', SDL_AUDIO_DRIVER='dummy')
    bad = 0
    for what, args, extra in (('shader compiler', ['--shaderc-selftest'], {}),
                              ('controllers', [], {'SMO_PAD_SELFTEST': '1'})):
        r = subprocess.run([exe] + args, cwd=OUT, env=dict(env, **extra), capture_output=True, text=True, timeout=120)
        last = (r.stdout + r.stderr).strip().splitlines()[-1:] or ['(no output)']
        print('%-16s exit %d: %s' % (what, r.returncode, last[0]))
        bad += r.returncode != 0
    print('PASS' if not bad else 'FAIL')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()

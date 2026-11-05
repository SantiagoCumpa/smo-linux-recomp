"""Link the runnable program: build/bin/libsmo_game.so (lifted main + game_shim.c) and build/bin/smo_native (host
runtime + gamedll.c, rpath $ORIGIN); adapted from ssbu-recomp's build.py `link` step (GPL-3.0-or-later). Linux only.

    python tools/lifter/build_exe.py [--exe smo_native] [--force]

Needs: build/obj/main/*.o (build_main.py), build/gen/* (gen_tables.py, gen_sdktest.py), clang + lld, and through
pkg-config: sdl3 vulkan shaderc libavcodec libavformat libavutil libswscale libswresample freetype2 fontconfig
(Arch: pacman -S clang lld sdl3 vulkan-headers vulkan-icd-loader shaderc ffmpeg freetype2 fontconfig).
Host files are recompiled when they or any runtime header / generated file changed. Prints every compiler error.
After linking, `smo_native --shaderc-selftest` must compile one vertex shader to SPIR-V.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402
GEN = T.build('gen')
LIFT = T.build('lift', 'main')
OBJ_GAME = T.build('obj', 'main')
BIN = T.build('bin')
CLANG = T.CLANG
PKGS = ['sdl3', 'vulkan', 'shaderc', 'libavcodec', 'libavformat', 'libavutil', 'libswscale', 'libswresample',
        'freetype2', 'fontconfig', 'libcrypto']


def pkg_config(*args):
    r = subprocess.run(['pkg-config'] + list(args) + PKGS, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('pkg-config %s failed: %s' % (' '.join(args), r.stderr.strip()))
    return r.stdout.split()


# The host runtime's build units by subsystem folder (runtime/<folder>/<file>). Not listed: nvn_vk_draw.c,
# nvn_vk_pipelines.c and nvn_vk_watchdog.c (included by nvn_vk.c) and game_shim.c (part of the game library).
HOST_SOURCES = {
    'core': ['plat.c', 'hostrt.c', 'boot.c', 'gamedll.c', 'sdklift_rt.c', 'hitch.c'],
    'libc': ['clib.c', 'nnlibc2.c', 'libcxx.c', 'nncxxlib.c', 'nncxxrt.c', 'nniostream.c', 'nnunwind.c'],
    'os': ['os_threads.c', 'nnsdk.c', 'nntime.c', 'nnmisc.c', 'smo_nn.c'],
    'fs': ['nnfs.c', 'nnfsdir.c'],
    'gfx': ['nvn_stub.c', 'nvn_gl.c', 'nvn_vk.c', 'nvn_misc.c', 'nvn_handles.c', 'nvn_layout.c', 'mx_translate.c',
            'tex_decode.c', 'display.c', 'gfx_options.c', 'vk_shaderc_shim.c', 'gl_absent.c', 'perf_stats.c'],
    'audio': ['nnaudio.c', 'smo_audio.c'],
    'input': ['nnhid.c', 'nnpad_sdl.c'],
    'services': ['nnservices.c', 'nnapplet.c', 'nnalbum.c', 'nnnet.c', 'nnpl.c', 'nnfontll.c', 'nnmovie.c', 'nnmovie_ff.c'],
    'game': ['smo_nerve.c', 'smo_fast.c', 'smo_selftest.c', 'smo_ui.c', 'smo_amiibo.c', 'smo_guide.c', 'smo_settings.c'],
}
INCLUDED = [T.runtime('gfx', n) for n in ('nvn_vk_draw.c', 'nvn_vk_pipelines.c', 'nvn_vk_watchdog.c')]
GEN_SOURCES = ['sdkdata.c', 'sdklift_gen.c']
# RECOMP_HOST_OPT (default -O2) and RECOMP_ARCH (as build_main.py: e.g. -march=native) for the runtime; a change of them
# recompiles nothing by itself: delete build/obj/host (or touch a runtime header) first
COMMON = ['-w', '-fno-strict-aliasing', '-ffp-contract=off', '-D_GNU_SOURCE'] + os.environ.get('RECOMP_ARCH', '').split()
HOST_FLAGS = None                                       # filled in by host_flags() (needs pkg-config)
LIBS = ['-lpthread', '-ldl', '-lm']


def host_flags():
    return ([os.environ.get('RECOMP_HOST_OPT', '-O2'), '-g1'] + COMMON + T.runtime_includes() + ['-I', GEN, '-I', LIFT,
            '-Dfn_table=(*game_fn_table_p)', '-Dfn_table_count=(*game_fn_table_count_p)',
            '-Ddyn_syms=(*game_dyn_syms_p)', '-Ddyn_sym_count=(*game_dyn_sym_count_p)'] + pkg_config('--cflags'))


def build_game_dll(force):
    dll = os.path.join(BIN, T.GAME_LIB)
    shim = T.runtime('core', 'game_shim.c')
    objs = sorted(glob.glob(os.path.join(OBJ_GAME, '*' + T.OBJ_EXT)))
    if not objs:
        raise SystemExit('no objects in %s: run tools/lifter/build_main.py first' % OBJ_GAME)
    newest = max([os.path.getmtime(o) for o in objs] + [os.path.getmtime(shim)])
    if not force and os.path.exists(dll) and os.path.getmtime(dll) >= newest:
        print('game library up to date: %s' % dll)
        return 0
    rsp = os.path.join(BIN, 'smo_game.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
    t0 = time.time()
    r = subprocess.run([CLANG, '-shared', '-fPIC', '-fvisibility=hidden', '-fuse-ld=lld', '-O1', '-w'] + T.runtime_includes()
                       + ['-I', LIFT, shim, '@' + rsp, '-Wl,-z,now', '-Wl,--no-undefined', '-Wl,-soname,' + T.GAME_LIB, '-lm', '-o', dll],
                       capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if r.returncode:
        print(out[-4000:])
        print('could not link %s' % dll)
        return 1
    print('linked %s: %.0f MB in %.0fs' % (dll, os.path.getsize(dll) / 2 ** 20, time.time() - t0))
    return 0


def compile_host(job):
    src, obj = job
    r = subprocess.run([CLANG, '-c'] + HOST_FLAGS + [src, '-o', obj], capture_output=True, text=True)
    return src, r.returncode, (r.stdout + r.stderr)


def main():
    global HOST_FLAGS
    force = '--force' in sys.argv
    exe = T.strip_exe(sys.argv[sys.argv.index('--exe') + 1]) if '--exe' in sys.argv else T.EXE
    HOST_FLAGS = host_flags()
    os.makedirs(BIN, exist_ok=True)
    if build_game_dll(force):
        return 1
    objdir = T.build('obj', 'host', exe)
    os.makedirs(objdir, exist_ok=True)
    deps = (glob.glob(os.path.join(T.RUNTIME, '**', '*.h'), recursive=True) +
            glob.glob(os.path.join(T.RUNTIME, '**', '*.inc'), recursive=True) +
            glob.glob(os.path.join(GEN, '*.h')) + INCLUDED + [os.path.abspath(__file__)])
    newest_dep = max(os.path.getmtime(p) for p in deps)
    objs, jobs = [], []
    sources = [T.runtime(d, n) for d, names in HOST_SOURCES.items() for n in names]
    for src in sources + [os.path.join(GEN, n) for n in GEN_SOURCES]:
        obj = os.path.join(objdir, os.path.basename(src)[:-2] + T.OBJ_EXT)
        objs.append(obj)
        if force or not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), newest_dep):
            jobs.append((src, obj))
    t0 = time.time()
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 8) as pool:
        for src, code, err in pool.map(compile_host, jobs):
            if code:
                failed += 1
                print('FAILED %s\n%s' % (os.path.basename(src), err[-6000:]))
    print('host files: %d compiled, %d failed (%.0fs)' % (len(jobs), failed, time.time() - t0))
    if failed:
        return 1
    out = os.path.join(BIN, exe)
    tmp = out + '.new'                                   # a running copy keeps its file: link beside it, then rename
    r = subprocess.run([CLANG, '-fuse-ld=lld'] + objs + [os.path.join(BIN, T.GAME_LIB), "-Wl,-rpath,$ORIGIN",
                        '-Wl,-Map=' + out + '.map'] + pkg_config('--libs') + LIBS + ['-o', tmp], capture_output=True, text=True)
    if r.returncode:
        print((r.stdout + r.stderr)[-8000:])
        return 1
    os.replace(tmp, out)
    print('linked %s (%.1f MB)' % (out, os.path.getsize(out) / 2 ** 20))
    return shader_selftest(out)


def shader_selftest(exe):
    """The executable's own shaderc must compile one vertex shader to SPIR-V."""
    r = subprocess.run([exe, '--shaderc-selftest'], capture_output=True, text=True)
    print((r.stdout + r.stderr).strip())
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())

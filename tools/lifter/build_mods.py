"""Build the default mods: defaults/mods/<id>/ -> build/mods/<id>/ (tools/lifter/build_exe.py runs this after linking).

    python tools/lifter/build_mods.py

Every file of a default mod is copied (mod.ini, romfs/, ...) except src/; a mod whose mod.ini names a `library` has its
src/ C files compiled into that shared library (native code, against this build's runtime headers and build/gen/image.h:
the game's addresses), only when a source or header is newer than it. Each folder made here gets a `.default` marker:
tools/make_game_folder.py replaces those in a game folder's mods/ every time and leaves the player's own mods alone, and
a folder whose default mod no longer exists is removed. A build run from the repository finds its mods in build/mods
(the runtime's <exe folder>/../mods). runtime/core/mods.c reads them; runtime/core/smo_mod_api.h is what code sees.
"""
import glob
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import target as T  # noqa: E402

SRC = os.path.join(T.REPO, 'defaults', 'mods')
OUT = T.build('mods')
CLANG = os.environ.get('CC_CLANG', 'clang')


def manifest(path):
    keys = {}
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.strip()
            if not line or line[0] in ';#[' or '=' not in line:
                continue
            k, v = line.split('=', 1)
            keys[k.strip().lower()] = v.split(' ;')[0].strip()
    return keys


def copy_files(src, dst):
    """The mod's files but src/ (copied when newer)."""
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        if rel == 'src' or rel.startswith('src' + os.sep):
            continue
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            a, b = os.path.join(root, f), os.path.join(dst, rel, f)
            if not os.path.exists(b) or os.path.getmtime(a) > os.path.getmtime(b):
                shutil.copy2(a, b)


def build_library(mod_id, src_dir, lib):
    sources = sorted(glob.glob(os.path.join(src_dir, '*.c')))
    if not sources:
        print('mods: %s names the library %s but has no src/*.c' % (mod_id, lib))
        return False
    deps = sources + glob.glob(os.path.join(T.RUNTIME, 'core', '*.h')) + glob.glob(T.build('gen', '*.h'))
    out = os.path.join(OUT, mod_id, lib)
    if os.path.exists(out) and os.path.getmtime(out) >= max(os.path.getmtime(d) for d in deps):
        return True
    cmd = ([CLANG, '-shared', '-fPIC', '-O2', '-g1', '-fvisibility=hidden', '-w', '-D_GNU_SOURCE'] + T.runtime_includes()
           + ['-I', T.build('gen')] + sources + ['-o', out + '.new'])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print('mods: %s: compiling %s failed:\n%s' % (mod_id, lib, (r.stdout + r.stderr)[-3000:]))
        return False
    os.replace(out + '.new', out)
    print('mods: %s: built %s' % (mod_id, os.path.relpath(out, T.REPO)))
    return True


def main():
    os.makedirs(OUT, exist_ok=True)
    ok = True
    ids = sorted(d for d in os.listdir(SRC) if os.path.isdir(os.path.join(SRC, d)))
    for mod_id in ids:
        src = os.path.join(SRC, mod_id)
        dst = os.path.join(OUT, mod_id)
        os.makedirs(dst, exist_ok=True)
        copy_files(src, dst)
        open(os.path.join(dst, '.default'), 'w').write('a default mod of the repository (defaults/mods); made again by the build\n')
        lib = manifest(os.path.join(src, 'mod.ini')).get('library')
        if lib:
            ok = build_library(mod_id, os.path.join(src, 'src'), lib) and ok
    for d in os.listdir(OUT):                  # default mods that were removed from defaults/mods
        if d not in ids and os.path.exists(os.path.join(OUT, d, '.default')):
            shutil.rmtree(os.path.join(OUT, d))
            print('mods: %s removed (no longer a default mod)' % d)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

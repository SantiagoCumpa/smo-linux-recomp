"""Build the game and put everything it needs in one folder that runs by itself (the layout of totk-pc-recomp's
install folder), outside the build tree:

    python tools/make_game_folder.py [--out smo] [--no-build] [--refresh-romfs] [--refresh-cache] [--link-romfs]
                                     [--language es]

The folder (default: smo/ at the repository root; ignored by git, it holds your game's files):

    smo/
      smo                 the launcher: run it (or double-click it) to play
      bin/                smo_native and libsmo_game.so; the game runs with this as its folder
      game/elf/           main.elf and sdk.elf (and the other modules); game/exefs/main.npdm
      romfs/              the game's data (from your dump)
      manual/             the electronic manual, if your dump has it
      config/             graphics.ini, controllers.ini (taken from build/config the first time, then yours)
      userdata/           saves (taken from build/userdata the first time, then never touched)
      cache/              shader and pipeline caches (vk/, nvidia/, mesa/; seeded from build/cache the first time)
      logs/               smo.log of the last run, smo.1.log .. smo.4.log of the runs before it
      tmp/, run/          the game's temporary files
      smo-folder.json     what built this folder: commit, date, the game version
      smo.env             settings the launcher reads (--language: SMO_LANGUAGE)

The runtime finds everything relative to bin/ (romfs, userdata and config through its exe folder, the Vulkan cache
as ../cache/vk from the working folder), so the folder can be moved anywhere. Making it again updates bin/ and game/
and keeps romfs/, config/, userdata/ and cache/ (--refresh-romfs, --refresh-cache to take them again from build/).
Copies use reflinks where the file system has them (btrfs, XFS): no extra space and no time for the 5 GB of romfs.
--link-romfs makes romfs/ a symbolic link to build/romfs instead (the folder then depends on the dump staying where it is).
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

LANGUAGES = ('ja', 'en-US', 'en-GB', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'zh-Hans', 'zh-Hant', 'ko', 'nl', 'ru')   # the game's (nnmisc.c)

LAUNCHER = r'''#!/usr/bin/env bash
# Super Mario Odyssey (smo-recomp): run the game from this folder. Arguments are passed to the game.
# SMO_PRIME=0 runs on the default GPU instead of the discrete one (NVIDIA PRIME offload).
set -e
here="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
mkdir -p "$here/logs" "$here/tmp" "$here/cache/nvidia" "$here/cache/mesa" "$here/cache/vk" "$here/run"
for i in 3 2 1; do [ -f "$here/logs/smo.$i.log" ] && mv -f "$here/logs/smo.$i.log" "$here/logs/smo.$((i + 1)).log"; done
[ -f "$here/logs/smo.log" ] && mv -f "$here/logs/smo.log" "$here/logs/smo.1.log"
export XDG_CACHE_HOME="$here/cache"
export __GL_SHADER_DISK_CACHE=1 __GL_SHADER_DISK_CACHE_PATH="$here/cache/nvidia" __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1
export MESA_SHADER_CACHE_DIR="$here/cache/mesa"
export TMPDIR="$here/tmp"
[ -f "$here/smo.env" ] && . "$here/smo.env"      # settings of this folder (SMO_LANGUAGE=...), written by the setup
if [ "${SMO_PRIME:-1}" != 0 ]; then
    export __NV_PRIME_RENDER_OFFLOAD="${__NV_PRIME_RENDER_OFFLOAD:-1}" __VK_LAYER_NV_optimus="${__VK_LAYER_NV_optimus:-NVIDIA_only}"
    export __GLX_VENDOR_LIBRARY_NAME="${__GLX_VENDOR_LIBRARY_NAME:-nvidia}"
fi
cd "$here/bin"
exec ./smo_native ../game/elf/main.elf 0 "$@" >"$here/logs/smo.log" 2>&1
'''

README = '''Super Mario Odyssey, recompiled (smo-recomp)

Play: run ./smo in this folder (from a terminal or your file manager).

  config/graphics.ini, config/controllers.ini   settings (F1 in the game opens the help)
  userdata/save/                                your saves: back this folder up
  logs/smo.log                                  the log of the last run (attach it to a bug report)
  cache/                                        shader caches; deleting it only makes the next start slower

This folder holds files from your own copy of the game: do not share it.
Made by tools/make_game_folder.py of the smo-recomp repository; run it again to update the game (saves, settings and
caches are kept).
'''


def run(cmd):
    print('  $ ' + ' '.join(cmd))
    subprocess.run(cmd, check=True)


def copy_tree(src, dst):
    """Copy src (file or folder, symbolic links followed) to dst, replacing dst; reflinks where possible."""
    if os.path.lexists(dst):
        shutil.rmtree(dst) if os.path.isdir(dst) and not os.path.islink(dst) else os.remove(dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    run(['cp', '-rL', '--preserve=timestamps', '--reflink=auto', src, dst])


def git_commit():
    try:
        return subprocess.run(['git', '-C', REPO, 'describe', '--always', '--dirty'], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return 'unknown'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', default=os.path.join(REPO, 'smo'), help='the game folder (default: smo/ in the repository)')
    ap.add_argument('--no-build', action='store_true', help='do not run tools/lifter/build_exe.py first')
    ap.add_argument('--refresh-romfs', action='store_true', help='take romfs/ and manual/ again from build/')
    ap.add_argument('--refresh-cache', action='store_true', help='take the shader caches again from build/cache')
    ap.add_argument('--link-romfs', action='store_true', help='romfs/ as a symbolic link to build/romfs (no copy)')
    ap.add_argument('--language', choices=LANGUAGES, help="the game's language (default: the system's, from LANG)")
    a = ap.parse_args()
    out = os.path.abspath(a.out)

    if not a.no_build:
        print('building the game (tools/lifter/build_exe.py)')
        run([sys.executable, os.path.join(REPO, 'tools', 'lifter', 'build_exe.py')])

    bin_dir = T.build('bin')
    exe, lib = os.path.join(bin_dir, T.EXE), os.path.join(bin_dir, 'libsmo_game.so')
    elf_dir = os.path.dirname(T.elf('main'))
    npdm = os.path.join(os.path.dirname(elf_dir), 'exefs', 'main.npdm')
    romfs = T.build('romfs')
    needed = [(exe, 'the game (tools/lifter/build_exe.py)'), (lib, 'the game library (tools/lifter/build_exe.py)'),
              (os.path.join(elf_dir, 'main.elf'), 'main.elf (scripts/setup.py import)'),
              (os.path.join(elf_dir, 'sdk.elf'), 'sdk.elf (scripts/setup.py import)'),
              (npdm, 'main.npdm (scripts/setup.py import)'), (os.path.join(romfs, 'SystemData'), 'the RomFS (scripts/setup.py import)')]
    missing = [what for path, what in needed if not os.path.exists(path)]
    if missing:
        sys.exit('make_game_folder: missing: ' + '; '.join(missing))
    if os.path.abspath(out) in (os.path.abspath(REPO), os.path.abspath(T.build())):
        sys.exit('make_game_folder: --out must be a folder of its own')

    print('game folder: ' + out)
    os.makedirs(out, exist_ok=True)

    # the program: always replaced
    os.makedirs(os.path.join(out, 'bin'), exist_ok=True)
    for f in (exe, lib):
        dst = os.path.join(out, 'bin', os.path.basename(f))
        tmp = dst + '.new'                       # a running copy keeps its file: write beside it, then rename
        shutil.copy2(f, tmp)
        os.replace(tmp, dst)
    os.chmod(os.path.join(out, 'bin', T.EXE), 0o755)
    for name in sorted(os.listdir(elf_dir)):
        if name.endswith('.elf'):
            copy_tree(os.path.join(elf_dir, name), os.path.join(out, 'game', 'elf', name))
    copy_tree(npdm, os.path.join(out, 'game', 'exefs', 'main.npdm'))
    functions = T.build('functions', 'main_functions.tsv')      # names for the debug logs (SMO_NERVELOG), small
    if os.path.exists(functions):
        copy_tree(functions, os.path.join(out, 'functions', 'main_functions.tsv'))

    # the game's data: kept unless asked
    dst_romfs = os.path.join(out, 'romfs')
    if a.link_romfs:
        if os.path.lexists(dst_romfs):
            shutil.rmtree(dst_romfs) if os.path.isdir(dst_romfs) and not os.path.islink(dst_romfs) else os.remove(dst_romfs)
        os.symlink(os.path.realpath(romfs), dst_romfs)
    elif a.refresh_romfs or not os.path.isdir(dst_romfs) or os.path.islink(dst_romfs):
        print('copying the RomFS (reflinks where the file system has them)')
        copy_tree(romfs, dst_romfs)
    manual = T.build('manual')
    if os.path.isdir(manual) and (a.refresh_romfs or not os.path.isdir(os.path.join(out, 'manual'))):
        copy_tree(manual, os.path.join(out, 'manual'))

    # the player's: taken the first time only
    for sub in ('config', 'userdata'):
        src, dst = T.build(sub), os.path.join(out, sub)
        if not os.path.isdir(dst):
            if os.path.isdir(src):
                copy_tree(src, dst)
                for name in os.listdir(dst):             # the save's work folders of a run that was killed
                    if name.startswith('save.work.'):
                        shutil.rmtree(os.path.join(dst, name), ignore_errors=True)
            else:
                os.makedirs(dst)
    for sub in ('vk', 'nvidia', 'mesa'):
        src, dst = T.build('cache', sub), os.path.join(out, 'cache', sub)
        if os.path.isdir(src) and (a.refresh_cache or not os.path.isdir(dst)):
            print('copying the shader cache ' + sub)
            copy_tree(src, dst)
    for sub in ('logs', 'tmp', 'run', os.path.join('cache', 'vk'), os.path.join('cache', 'nvidia'), os.path.join('cache', 'mesa')):
        os.makedirs(os.path.join(out, sub), exist_ok=True)

    launcher = os.path.join(out, 'smo')
    with open(launcher, 'w', encoding='utf-8') as f:
        f.write(LAUNCHER)
    os.chmod(launcher, 0o755)
    env_file = os.path.join(out, 'smo.env')
    if a.language:
        with open(env_file, 'w', encoding='utf-8') as f:
            f.write('# settings of this game folder, read by ./smo\nexport SMO_LANGUAGE=%s\n' % a.language)
    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8') as f:
        f.write(README)
    with open(os.path.join(out, 'smo-folder.json'), 'w', encoding='utf-8') as f:
        json.dump({'commit': git_commit(), 'made': datetime.datetime.now().isoformat(timespec='seconds'),
                   'game version': T.VERSION, 'romfs': 'link' if a.link_romfs else 'copy'}, f, indent=2)
        f.write('\n')
    print('done: run %s' % launcher)


if __name__ == '__main__':
    main()

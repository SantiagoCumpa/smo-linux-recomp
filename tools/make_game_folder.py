"""Build the game and put everything it needs in one folder that runs by itself (the layout of totk-pc-recomp's
install folder), outside the build tree:

    python tools/make_game_folder.py [--out smo] [--no-build] [--refresh-romfs] [--refresh-cache] [--link-romfs]
                                     [--language es]

The folder (default: smo/ at the repository root; ignored by git, it holds your game's files):

    smo/
      smo                 run it (or double-click it, or smo.desktop) to play; F1 in the game opens the settings menu
                          (./smo --compile-shaders: the material shaders ahead, see below)
      smo.desktop         a menu entry: copy it to ~/.local/share/applications to have the game in the app menu
      bin/                smo_native and libsmo_game.so; the game runs with this as its folder
      game/elf/           main.elf and sdk.elf (and the other modules); game/exefs/main.npdm
      romfs/              the game's data (from your dump)
      manual/             the electronic manual, if your dump has it
      config/             graphics.ini, controllers.ini (from the repository's defaults/config the first time, then yours)
      userdata/           saves (empty the first time, then never touched)
      mods/               the mods: the default ones (from the repository's defaults/mods, made again each time) and
                          yours (RomFS mods in the emulators' layout, mods/<name>/romfs/...); config/mods.ini: on / off
      cache/              shader and pipeline caches (vk/, nvidia/, mesa/; seeded from build/cache the first time)
      logs/               smo.log of the last run, smo.1.log .. smo.4.log of the runs before it
      tmp/, run/          the game's temporary files
      smo-folder.json     what built this folder: commit, date, the game version
      smo.env             environment for the game, read by ./smo when it exists (e.g. export SMO_LANGUAGE=es)

The runtime finds everything relative to bin/ (romfs, userdata and config through its exe folder, the Vulkan cache
as ../cache/vk from the working folder), so the folder can be moved anywhere. Making it again updates bin/ and game/
and keeps romfs/, config/, userdata/ and cache/ (--refresh-romfs, --refresh-cache to take them again from build/).
./smo --compile-shaders translates and compiles the material shaders of every kingdom into cache/ (a few minutes the first
time; scripts/install.py runs it): the game loads them while you play, and a shader made then is a stutter.
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

RUN_SCRIPT = r'''#!/usr/bin/env bash
# Super Mario Odyssey (smo-recomp): run the game from this folder. Arguments are passed to the game. Its settings are in the
# game's own menu (F1, or a pad's HOME button), kept in config/graphics.ini. The log goes to logs/smo.log (the four before
# it: smo.1.log .. smo.4.log) and, when started from a terminal, to the terminal as well.
# SMO_PRIME=0 runs on the default GPU instead of the discrete one (NVIDIA PRIME offload).
set -e
here="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
mkdir -p "$here/logs" "$here/tmp" "$here/cache/nvidia" "$here/cache/mesa" "$here/cache/vk" "$here/run"
export XDG_CACHE_HOME="$here/cache"
export __GL_SHADER_DISK_CACHE=1 __GL_SHADER_DISK_CACHE_PATH="$here/cache/nvidia" __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1
export MESA_SHADER_CACHE_DIR="$here/cache/mesa"
export TMPDIR="$here/tmp"
export SMO_LOG_TAGS="${SMO_LOG_TAGS:-1}"          # [INFO] / [WARN] / [ERROR] in front of every line of the log (runtime/core/log.c)
[ -f "$here/smo.env" ] && . "$here/smo.env"      # environment of this folder, if any (the settings menu keeps SMO_LANGUAGE in it)
if [ "${SMO_PRIME:-1}" != 0 ]; then
    export __NV_PRIME_RENDER_OFFLOAD="${__NV_PRIME_RENDER_OFFLOAD:-1}" __VK_LAYER_NV_optimus="${__VK_LAYER_NV_optimus:-NVIDIA_only}"
    export __GLX_VENDOR_LIBRARY_NAME="${__GLX_VENDOR_LIBRARY_NAME:-nvidia}"
fi
cd "$here/bin"
if [ "${1:-}" = "--list-gpus" ]; then
    # the GPUs of this PC as Vulkan sees them, one a line: gpu <n> <nvidia|intel|amd|other> <name> <driver> <ok | why not>
    SMO_LOG_TAGS=0 exec ./smo_native --list-gpus
fi
if [ "${1:-}" = "--compile-shaders" ]; then
    # the material shaders of every kingdom, translated and compiled ahead (the game loads them while you play: without
    # this, the first visit of a place can stutter). No window; the game does not start. Log: logs/compile-shaders.log
    # ./smo --compile-shaders [all | nvidia intel amd ...]: the GPUs to compile for (default: every GPU of the PC)
    shift
    gpus="all"
    while [ $# -gt 0 ] && [[ "$1" =~ ^(all|nvidia|intel|amd)$ ]]; do if [ "$gpus" = all ]; then gpus="$1"; else gpus="$gpus,$1"; fi; shift; done
    export SMO_PRECOMPILE_GPUS="$gpus"
    # every GPU of the PC in turn (SMO_PRECOMPILE_ALL: the program starts again for the next one), so that switching GPU
    # in the settings menu finds its shaders ready too. Which GPUs there are: Vulkan says (./smo --list-gpus)
    list="$(SMO_LOG_TAGS=0 ./smo_native --list-gpus 2>/dev/null | grep "^gpu	")"
    usable="$(printf '%s\n' "$list" | awk -F'\t' '$6 == "ok"')"
    label() { case "$1" in nvidia) echo NVIDIA ;; intel) echo Intel ;; amd) echo AMD ;; *) echo "$1" ;; esac; }
    if [ "${SMO_PROGRESS:-}" != 1 ]; then
        echo "GPUs found on this PC:"
        [ -n "$list" ] || echo "  none (no Vulkan driver answered)"
        printf '%s\n' "$list" | while IFS='	' read -r _ _ kind name drv why; do
            [ -n "$kind" ] || continue
            if [ "$why" = ok ]; then echo "  $name ($(label "$kind"), driver $drv)"; else echo "  $name: not used ($why)"; fi
        done
    fi
    if [ -z "$usable" ]; then echo "  No GPU the game can use (Vulkan 1.3): nothing compiled"; exit 3; fi
    kinds="$(printf '%s\n' "$usable" | cut -f3 | awk '!seen[$0]++' | paste -sd, -)"
    if [ "$gpus" != all ]; then
        keep=""
        for g in ${gpus//,/ }; do
            if [[ ",$kinds," == *",$g,"* ]]; then keep="${keep:+$keep,}$g"; else [ "${SMO_PROGRESS:-}" = 1 ] || echo "  (no $(label "$g") GPU on this PC: left out)"; fi
        done
        if [ -z "$keep" ]; then echo "  no GPU of this PC is one of \"$gpus\": nothing compiled"; exit 3; fi
        gpus="$keep"; export SMO_PRECOMPILE_GPUS="$gpus"
    fi
    if [ "$gpus" = all ]; then what="$kinds"; else what="$gpus"; fi
    what="$(for g in ${what//,/ }; do label "$g"; done | paste -sd, - | sed 's/,/, /g')"
    [ "${SMO_PROGRESS:-}" = 1 ] && echo "@found $what"
    [ "${SMO_PROGRESS:-}" = 1 ] || echo "Compiling shaders for: $what (the first time: a few minutes each)"
    SMO_PRECOMPILE=1 SMO_PRECOMPILE_ALL=1 ./smo_native ../game/elf/main.elf 0 "$@" 2>&1 | tee "$here/logs/compile-shaders.log" | while IFS= read -r line; do
        line="${line#\[INFO\] }"; line="${line#\[WARN\] }"; line="${line#\[ERROR\] }"      # (the log's level tags)
        if [ "${SMO_PROGRESS:-}" = 1 ]; then          # the installer (scripts/install.py) draws its own progress line
            [[ "$line" =~ ^precompile:\ GPU\ ([0-9]+)\ of\ ([0-9]+) ]] && echo "@gpu ${BASH_REMATCH[1]} ${BASH_REMATCH[2]} ${line#*: GPU * of *: }"
            [[ "$line" =~ ^precompile:\ [0-9]+%\ \(([0-9]+)\ of\ ([0-9]+) ]] && echo "@progress shaders ${BASH_REMATCH[1]} ${BASH_REMATCH[2]}"
            [[ "$line" == "precompile: done"* ]] && echo "${line#precompile: }"
            continue
        fi
        case "$line" in
            "precompile: GPU "*) printf '  %s\n' "${line#precompile: }" ;;
            "precompile: "*%*) printf '\r    %s   ' "${line#precompile: }" ;;
            "precompile: done"*) printf '\r    %s\n' "${line#precompile: }" ;;
        esac
    done
    status=${PIPESTATUS[0]}
    tags='^(\[[A-Z]+\] )?'
    if grep -Eq "${tags}precompile: no GPU" "$here/logs/compile-shaders.log"; then grep -E "${tags}precompile: no GPU" "$here/logs/compile-shaders.log" | sed -E "s/${tags}precompile: /  /"; exit 3; fi
    grep -Eq "${tags}precompile: done" "$here/logs/compile-shaders.log" || { echo "Compiling shaders failed: see $here/logs/compile-shaders.log"; exit 1; }
    exit "$status"
fi
for i in 3 2 1; do [ -f "$here/logs/smo.$i.log" ] && mv -f "$here/logs/smo.$i.log" "$here/logs/smo.$((i + 1)).log"; done
[ -f "$here/logs/smo.log" ] && mv -f "$here/logs/smo.log" "$here/logs/smo.1.log"
if [ -t 1 ]; then
    # started from a terminal: the log is shown there as well as written to logs/smo.log. Ctrl+C (or a TERM) goes to the
    # whole group: the game closes as when its window is closed; the copy (tee) ignores it and ends when the game's
    # output does (killed first, it would kill the game at its next line), and this script waits for both
    echo "log: $here/logs/smo.log"
    trap ':' INT TERM                              # (a handler, not ignored: the game gets the signal as usual)
    ./smo_native ../game/elf/main.elf 0 "$@" 2>&1 | ( trap '' INT TERM; exec tee "$here/logs/smo.log" )
    exit "${PIPESTATUS[0]}"
fi
exec ./smo_native ../game/elf/main.elf 0 "$@" >"$here/logs/smo.log" 2>&1
'''

DESKTOP = '''[Desktop Entry]
Type=Application
Name=Super Mario Odyssey
Comment=smo-recomp
Exec="{folder}/smo"
Path={folder}
Icon=applications-games
Terminal=false
Categories=Game;
'''

README = '''Super Mario Odyssey, recompiled (smo-recomp)

Play: run ./smo in this folder (from a terminal or your file manager). In the game, F1 (or a pad's HOME button) opens
the settings menu: resolution (up to 4K), full screen or window, vertical sync, performance overlay, GPU, language...
smo.desktop is a menu entry for this folder: copy it to ~/.local/share/applications.

Shaders: ./smo --compile-shaders prepares the shaders of every kingdom ahead, for every GPU of the PC (a few minutes
the first time; ./smo --list-gpus shows which GPUs it finds; ./smo --compile-shaders nvidia, or intel, amd: only those), so that a new place does not stutter the first time you see it. After a graphics driver update the
game does it again by itself when it starts (a small box with a bar in the window). Run it again after switching the
internal resolution between native and a scaled one (logs/smo.log says when it would help).

  config/graphics.ini, config/controllers.ini   settings (the settings menu writes graphics.ini)
  userdata/save/                                your saves: back this folder up
  mods/                                         mods that replace the game's files (see Mods below)
  logs/smo.log                                  the log of the last run (attach it to a bug report); started
                                                from a terminal, ./smo shows it there too
  cache/                                        shader caches; deleting it only makes the next start slower
                                                (then run ./smo --compile-shaders again)

Mods: with every mod off this is the console's game. The settings menu (F1, Mods) has a switch for each mod (used
from the next start; config/mods.ini keeps them). The program's own mods are already there: HD resolution,
Anisotropic filtering, Texture filtering, Asynchronous shaders (off) and Fast sampler sort. To add a mod made for emulators, put it in
its own folder under mods/ with its romfs/ folder inside (mods/<name>/romfs/...; a folder copied from an emulator's
load/0100000000010000/ works too). The order of config/mods.ini decides which wins when two replace the same file.
Only romfs/ is used: exefs/ code patches for emulators are not applied.

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

    # the player's: made empty the first time, then never touched. Not taken from build/config and build/userdata: those
    # are the repository's test settings and saves; the game writes graphics.ini and controllers.ini with their defaults
    # (full screen, the game's own 900p) at its first start
    for sub in ('config', 'userdata', 'mods'):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    # the default mods (defaults/mods, built into build/mods by tools/lifter/build_mods.py): made again in mods/ every time
    # (they are the program's, as bin/ is); the player's own mods there are left alone. Which are on: config/mods.ini.
    built = T.build('mods')
    if os.path.isdir(built):
        shipped = [d for d in sorted(os.listdir(built)) if os.path.exists(os.path.join(built, d, '.default'))]
        for d in shipped:
            copy_tree(os.path.join(built, d), os.path.join(out, 'mods', d))
        for d in os.listdir(os.path.join(out, 'mods')):
            if d not in shipped and os.path.exists(os.path.join(out, 'mods', d, '.default')):
                shutil.rmtree(os.path.join(out, 'mods', d))
                print('removed the default mod %s (no longer one)' % d)
    else:
        print('no build/mods: run tools/lifter/build_exe.py (it builds the default mods)')
    # defaults/config: the settings a new game folder starts with (a file the player already has is kept; the program
    # embeds the same text and writes it when a file is missing)
    for name in sorted(os.listdir(os.path.join(REPO, 'defaults', 'config'))):
        dst = os.path.join(out, 'config', name)
        if not os.path.exists(dst):
            shutil.copy2(os.path.join(REPO, 'defaults', 'config', name), dst)
    for sub in ('vk', 'nvidia', 'mesa'):
        src, dst = T.build('cache', sub), os.path.join(out, 'cache', sub)
        if os.path.isdir(src) and (a.refresh_cache or not os.path.isdir(dst)):
            print('copying the shader cache ' + sub)
            copy_tree(src, dst)
    for sub in ('logs', 'tmp', 'run', os.path.join('cache', 'vk'), os.path.join('cache', 'nvidia'), os.path.join('cache', 'mesa')):
        os.makedirs(os.path.join(out, sub), exist_ok=True)

    launcher = os.path.join(out, 'smo')
    with open(launcher + '.new', 'w', encoding='utf-8') as f:
        f.write(RUN_SCRIPT)
    os.chmod(launcher + '.new', 0o755)
    os.replace(launcher + '.new', launcher)
    # folders made with the GTK launcher (before the settings menu replaced it): the launcher, ./smo-run and SMO_LAUNCHER go
    shutil.rmtree(os.path.join(out, 'launcher'), ignore_errors=True)
    if os.path.exists(os.path.join(out, 'smo-run')):
        os.remove(os.path.join(out, 'smo-run'))
    env_file = os.path.join(out, 'smo.env')
    if os.path.exists(env_file):
        lines = [l for l in open(env_file, encoding='utf-8') if 'SMO_LAUNCHER=' not in l and (not a.language or 'SMO_LANGUAGE=' not in l)
                 and not (l.startswith('#') and 'smo-run' in l)]
        if not lines or not lines[0].startswith('#'):
            lines.insert(0, '# environment of this game folder, read by ./smo (the settings menu, F1, keeps SMO_LANGUAGE here)\n')
        with open(env_file, 'w', encoding='utf-8') as f:
            f.writelines(lines)
    with open(os.path.join(out, 'smo.desktop'), 'w', encoding='utf-8') as f:
        f.write(DESKTOP.format(folder=out))
    if a.language:                                       # where the settings menu keeps it
        ini = os.path.join(out, 'config', 'graphics.ini')
        lines = open(ini, encoding='utf-8').readlines() if os.path.exists(ini) else []
        lines = [l for l in lines if not l.split('=', 1)[0].strip().lower() == 'language']
        if lines and not lines[-1].endswith('\n'):
            lines[-1] += '\n'
        lines.append('language = %s\n' % a.language)
        with open(ini, 'w', encoding='utf-8') as f:
            f.writelines(lines)
    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8') as f:
        f.write(README)
    with open(os.path.join(out, 'smo-folder.json'), 'w', encoding='utf-8') as f:
        json.dump({'commit': git_commit(), 'made': datetime.datetime.now().isoformat(timespec='seconds'),
                   'game version': T.VERSION, 'romfs': 'link' if a.link_romfs else 'copy'}, f, indent=2)
        f.write('\n')
    print('done: run %s' % launcher)


if __name__ == '__main__':
    main()

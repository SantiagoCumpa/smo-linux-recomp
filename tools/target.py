"""Where the build's files are (used by the tools, scripts and tests).

The game is Super Mario Odyssey 1.4.1: the base game with its 1.4.1 update applied. Everything derived from your dump
goes under build/:
    build/dump/update   the program's modules (ExeFS, ELF conversions) and its control data
    build/<the rest>    romfs, manual, functions, lift, obj, gen, bin, run, ... laid out so that the runtime's paths
                        relative to its exe (../romfs, ../manual, ../userdata) work

Linux only (x86-64): clang + lld from the system (or $LLVM_ROOT/bin), objects .o, the game library libsmo_game.so and
the executable smo_native (no extension).
"""
import os
import shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = '1.4.1'                                       # the game version this project builds
BUILD = os.path.join(REPO, 'build')
DUMP = os.path.join(BUILD, 'dump', 'update')
RUNTIME = os.path.join(REPO, 'runtime')
# the host runtime's subsystem folders (runtime/<folder>); every one is an include directory of the runtime's code
RUNTIME_DIRS = ['core', 'libc', 'os', 'fs', 'gfx', 'audio', 'input', 'services', 'game']


def _llvm(tool):
    root = os.environ.get('LLVM_ROOT')
    if root:
        return os.path.join(root, 'bin', tool)
    return shutil.which(tool) or tool


CLANG = _llvm('clang')                                  # the C compiler of every build step (clang 15 or later)
OBJ_EXT = '.o'
EXE = 'smo_native'                                      # the program a person plays
GAME_LIB = 'libsmo_game.so'                             # the lifted game, loaded by the executable (rpath $ORIGIN)
CACHE = os.path.join(BUILD, 'cache')                    # driver and shader caches (XDG_CACHE_HOME of the runs)
TMP = os.path.join(BUILD, 'tmp')                        # temporary files of tools and tests (never /tmp)
RUNLOCKS = os.path.join(BUILD, 'run')                   # flock files: test "places" and console-core slots


def exe_path(name=EXE):
    """build/bin/<name>; a Windows-style name ('smo_test.exe') is accepted and the extension dropped."""
    if name.lower().endswith('.exe'):
        name = name[:-4]
    return os.path.join(BUILD, 'bin', name)


def strip_exe(name):
    return name[:-4] if name.lower().endswith('.exe') else name


def runtime(folder, name):
    """A file of the host runtime, e.g. runtime('gfx', 'mx_translate.c')."""
    return os.path.join(RUNTIME, folder, name)


def runtime_includes():
    """clang -I arguments for every runtime folder (the runtime's files include each other by bare name)."""
    return [arg for d in RUNTIME_DIRS for arg in ('-I', os.path.join(RUNTIME, d))]


def build(*parts):
    """A path in the build folder."""
    return os.path.join(BUILD, *parts)


def elf(module):
    """The converted module (main, sdk, subsdk0, rtld) of the program."""
    return os.path.join(DUMP, 'elf', module + '.elf')


def dump_version():
    """The display version of the extracted program (from its control data), or None."""
    p = os.path.join(DUMP, 'control.nacp')
    if not os.path.isfile(p):
        return None
    return open(p, 'rb').read()[0x3060:0x3070].split(b'\0')[0].decode('ascii', 'replace')

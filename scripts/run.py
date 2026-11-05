"""Run the built game for a while and summarise what happened.

    python scripts/run.py <name> [--seconds 30] [--exe smo_test] [--env NAME=VALUE ...] [--full]
    python scripts/run.py <name> --exe smo_native --seconds 0 [--userdata <folder>]      play: window, sound, no time limit
                          [--seq <file.seq>] [--userdata <folder>]

Copies build/bin/smo_native to the test name (only smo_native / smo_user open a visible window
and play sound; SMO_SHOW=1 shows a test exe), runs it from build/bin with the converted dump's main.elf and the time
limit, writes everything to build/run/<name>.log, then prints: the boot phases reached, the unimplemented imports
(name and call count), the strict check line (do-nothing handlers), frames presented, and a crash with its host frames named from the
linker map. --full prints the whole log instead.

--seq plays an input sequence file (SMO_SEQ, runtime/input/nnhid.c; one or more steps per line, '#' starts a comment), e.g.
tests/cap_kingdom.seq. --userdata gives the game its own save folder (SMO_USERDATA; created if missing): a new, empty
folder starts a new game, a copy of an earlier run's folder continues from there. Without it saves go to build/userdata.

A test copy first waits for one of SMO_MAX_GAMES places on this PC (default: one per 12 GB of memory, 2 on a 32 GB PC;
0 = no limit): a game in a stage takes 4-7 GB, and more test games than fit make the PC page, which slows every game
and can make graphics memory allocations fail. smo_native and smo_user never wait. A place is an flock on
build/run/place_<k>.lock (freed by the kernel when its launcher dies).

Linux: on a hybrid laptop the game runs on the discrete NVIDIA GPU (PRIME render offload: __NV_PRIME_RENDER_OFFLOAD=1,
__VK_LAYER_NV_optimus=NVIDIA_only, __GLX_VENDOR_LIBRARY_NAME=nvidia; SMO_PRIME=0 leaves them out), and the driver's and
Mesa's shader caches go into build/cache (XDG_CACHE_HOME, __GL_SHADER_DISK_CACHE_PATH, MESA_SHADER_CACHE_DIR).
"""
import argparse
import fcntl
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools', 'lifter'))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import mapaddr  # noqa: E402

PLAYED = ('smo_native', 'smo_user')     # the copies a person plays: a window, sound, no waiting for a place


def _total_memory():
    for line in open('/proc/meminfo'):
        if line.startswith('MemTotal:'):
            return int(line.split()[1]) * 1024
    return 16 * 2**30


def max_games():
    """SMO_MAX_GAMES, or one test game per 12 GB of the PC's memory (at least one)."""
    if os.environ.get('SMO_MAX_GAMES'):
        return max(int(os.environ['SMO_MAX_GAMES']), 0)
    return max(int(_total_memory() / 2**30 / 12), 1)


def _place_dir():
    sys.path.insert(0, os.path.join(REPO, 'tools'))
    import target as T
    os.makedirs(T.RUNLOCKS, exist_ok=True)
    return T.RUNLOCKS


def take_place(exe_name, poll=2.0, name='place_%d.lock'):
    """Waits until fewer than max_games() test games run on this PC and takes a place: an exclusive flock on
    build/run/place_<k>.lock, held until release_place (a launcher that dies frees its place: the kernel drops its locks).
    Returns the open file, or None for the played copies and with no limit."""
    n = max_games()
    if os.path.basename(exe_name).lower() in PLAYED or n == 0:
        return None
    folder = _place_dir()
    waited = None
    while True:
        for k in range(n):
            f = open(os.path.join(folder, name % k), 'a+')
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                f.close()
                continue
            if waited:
                print('run.py: place %d of %d free after %.0f s' % (k, n, time.time() - waited))
            return f
        if waited is None:
            waited = time.time()
            print('run.py: %d test games are running (SMO_MAX_GAMES=%d): waiting for one to end' % (n, n))
            sys.stdout.flush()
        time.sleep(poll)


def release_place(f):
    if f:
        fcntl.flock(f, fcntl.LOCK_UN)
        f.close()


def game_env(T, env):
    """PRIME offload to the discrete GPU and every cache inside build/cache."""
    cache = T.CACHE
    for sub in ('', 'nvidia', 'mesa'):
        os.makedirs(os.path.join(cache, sub), exist_ok=True)
    env.setdefault('XDG_CACHE_HOME', cache)
    env.setdefault('__GL_SHADER_DISK_CACHE', '1')
    env.setdefault('__GL_SHADER_DISK_CACHE_PATH', os.path.join(cache, 'nvidia'))
    env.setdefault('__GL_SHADER_DISK_CACHE_SKIP_CLEANUP', '1')
    env.setdefault('MESA_SHADER_CACHE_DIR', os.path.join(cache, 'mesa'))
    env.setdefault('TMPDIR', T.TMP)
    os.makedirs(T.TMP, exist_ok=True)
    if env.get('SMO_PRIME', '1') != '0':
        env.setdefault('__NV_PRIME_RENDER_OFFLOAD', '1')
        env.setdefault('__VK_LAYER_NV_optimus', 'NVIDIA_only')
        env.setdefault('__GLX_VENDOR_LIBRARY_NAME', 'nvidia')
    return env


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--exe', default='smo_test')
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--full', action='store_true')
    ap.add_argument('--seq', help='input sequence file (SMO_SEQ)')
    ap.add_argument('--userdata', help='save folder for this run (SMO_USERDATA)')
    ap.add_argument('--timeout', type=int, help='seconds after which a game still running is killed (default --seconds + 120; '
                                                'a run of several stages, SMO_STAGE_LIST, has the time limit per stage)')
    a = ap.parse_args()
    import target as T
    BIN, RUN = T.build('bin'), T.build('run')
    os.makedirs(RUN, exist_ok=True)
    os.makedirs(T.build('userdata'), exist_ok=True)
    if a.seq:
        steps = [line.split('#', 1)[0].strip() for line in open(a.seq, encoding='utf-8')]
        a.env.append('SMO_SEQ=' + ' '.join(s for s in steps if s))
    if a.userdata:
        os.makedirs(a.userdata, exist_ok=True)
        a.env.append('SMO_USERDATA=' + os.path.abspath(a.userdata))
    a.exe = T.strip_exe(a.exe)
    src = T.exe_path()
    exe = T.exe_path(a.exe)
    place = take_place(a.exe)
    try:
        if a.exe != T.EXE:
            tmp = exe + '.new'                           # a copy still running keeps its file: copy beside it, then rename
            shutil.copy2(src, tmp)
            os.chmod(tmp, 0o755)
            os.replace(tmp, exe)
        env = game_env(T, dict(os.environ))
        if a.exe != T.EXE:
            env.setdefault('SMO_UNFOCUSED', 'run')     # a test copy keeps running behind other windows (the game pauses while unfocused)
            env.setdefault('SMO_VK_PCACHE', '1')       # its own pipeline cache object: the driver's disk cache is kept per exe name
        for kv in a.env:
            k, v = kv.split('=', 1)
            env[k] = v
        log = os.path.join(RUN, a.name + '.log')
        with open(log, 'w', encoding='utf-8', errors='replace') as f:
            try:
                r = subprocess.run([exe, os.path.relpath(T.elf('main'), BIN), str(max(a.seconds, 0))], cwd=BIN, stdout=f,
                                   stderr=subprocess.STDOUT, env=env,      # 0: no time limit, until the window is closed
                                   timeout=a.timeout or (a.seconds + 120 if a.seconds > 0 else None))
                code = r.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout (killed)'
    finally:
        release_place(place)
    text = open(log, encoding='utf-8', errors='replace').read()
    if a.full:
        print(text)
        return
    lines = text.split('\n')
    print('log: %s   exit code: %s   lines: %d' % (log, code, len(lines)))
    for l in lines:
        if l.startswith(('[phase', 'strict check', 'display: ', 'nnMain returned', 'still running', 'GUEST TRAP', 'UNTRANSLATED',
                         'STRICT', 'memory heap', 'imports reached', 'main thread: last')) or 'frames presented' in l:
            print('  ' + l[:220])
    if 'unimplemented imports reached:' in text:
        part = text.split('unimplemented imports reached:', 1)[1].split('\n\n', 1)[0]
        print('unimplemented imports reached (calls, name):')
        for l in part.strip('\n').split('\n'):
            if l.strip():
                print('  ' + l.strip())
    m = re.search(r'HOST EXCEPTION.*?(?=\nimports reached|\Z)', text, re.S)
    if m:
        block = m.group(0)
        print('\n'.join('  ' + l for l in block.split('\n')[:4]))
        tables = {}
        for mod, off in re.findall(r'^\s+(\S+?)\+0x([0-9a-f]+)$', block, re.M):
            name = ''
            path = os.path.join(BIN, os.path.basename(mod))
            if os.path.basename(mod) in (a.exe, T.GAME_LIB) and os.path.exists(path):
                if path not in tables:
                    tables[path] = mapaddr.load(path)[1]
                name = mapaddr.name_at(tables[path], int(off, 16))
            print('    %s+0x%s %s' % (mod, off, name))
        for l in block.split('\n'):
            if l.strip().startswith('f_') or 'last guest function' in l:
                print('  ' + l.strip())


if __name__ == '__main__':
    main()

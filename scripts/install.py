"""From your own game files to a folder you play from, in one command (the CLI of totk-pc-recomp's setup):

    ./smo-recomp --in_dir <folder with the NSP files> --keys <prod.keys> --out_dir <game folder> [--language es]

    --in_dir    the base game and the 1.4.1 update of Super Mario Odyssey: two NSP files, or one NSP that carries
                both (any names; other files in the folder are ignored)
    --keys      your console's prod.keys
    --out_dir   the game folder to make (created; made again on the same folder, it keeps saves, settings, caches)
    --language  the game's language: ja en-US en-GB fr fr-CA de it es es-419 zh-Hans zh-Hant ko nl ru (default:
                your system's, from LANG; changed later in the game's settings menu, F1)
    --jobs N    compile jobs (default: all CPUs but two)
    --no-shaders  skip step 6 (compiling the shaders ahead; <out_dir>/smo --compile-shaders does it later)

Steps, each skipped when its result is already there (a run that stopped resumes):
    1. check     tools and libraries (scripts/setup.py's deps step)
    2. hactool   the NSP decrypter the repository carries (tools/bin/hactool, a static build)
    3. decrypt   the NSP files -> build/install/dump (tools/extract/nsp.py; about 5.5 GB, a minute)
    4. build     lift and compile the game from it (scripts/setup.py; the first time takes long)
    5. folder    the game folder (tools/make_game_folder.py): ./smo starts the game
    6. shaders   the material shaders of every kingdom, compiled ahead (<out_dir>/smo --compile-shaders; a few
                 minutes the first time, seconds when they are there)

No firmware is needed: the game's text uses its own fonts, and the system fonts stand in for the console's shared
fonts. Everything but the game folder stays inside the repository's build/ folder.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'extract'))
sys.path.insert(0, os.path.join(REPO, 'scripts'))
import target as T  # noqa: E402

PY = sys.executable
STATE = T.build('install', 'state.json')
DUMP = T.build('install', 'dump')
HACTOOL = os.path.join(REPO, 'tools', 'bin', 'hactool')   # carried by the repository (tools/build_hactool.py rebuilds it)
LANGUAGES = ('ja', 'en-US', 'en-GB', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'zh-Hans', 'zh-Hant', 'ko', 'nl', 'ru')


from progress_ui import Task, run_logged  # noqa: E402  (scripts/progress_ui.py: the live progress lines)

LOG = T.build('install', 'install.log')
DUMP_BYTES = 5.4e9                                    # what decrypting writes (the 1.4.1 game: ExeFS, RomFS, manual)
BUILD_STEPS = {                                       # scripts/setup.py's steps, as the progress lines name them
    'deps': 'Checking tools and libraries', 'import': 'Importing the game files', 'functions': "Finding the game's functions",
    'lift': 'Translating the game code to C', 'compile': 'Compiling the game', 'tables': 'Generating tables',
    'sdk': 'Translating the system code', 'link': 'Linking the program', 'check': 'Checking the build'}
PHASES = {'discover': 'finding code', 'write': 'writing C files', 'compile': 'files', 'shaders': 'programs'}


def fail(task, what, tail):
    task.finish(ok=False)
    print('\n%s failed. The last lines of its output:\n    %s\n\nThe whole log: %s\nFix the cause and run the same '
          'command again: it resumes here.' % (what, '\n    '.join(tail[-15:]), LOG))
    sys.exit(1)


def progress_line(task, line):
    """A tool's '@progress <phase> <done> <total>' line: the bar."""
    parts = line.split()
    if len(parts) == 4 and parts[0] == '@progress':
        done, total = int(parts[2]), max(1, int(parts[3]))
        task.update(done / total, '%s %d/%d' % (PHASES.get(parts[1], parts[1]), done, total))
        return True
    return False


def inputs_key(in_dir, keys):
    """What the decrypted dump was made from: the NSP files (name, size, time) and the keys file's hash."""
    h = hashlib.sha256()
    for name in sorted(os.listdir(in_dir)):
        if name.lower().endswith('.nsp'):
            st = os.stat(os.path.join(in_dir, name))
            h.update(('%s %d %d\n' % (name, st.st_size, int(st.st_mtime))).encode())
    h.update(open(keys, 'rb').read())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split('\n\n', 2)[2])
    ap.add_argument('--in_dir', '--in-dir', dest='in_dir', required=True, help='folder with the NSP files')
    ap.add_argument('--keys', required=True, help='your prod.keys')
    ap.add_argument('--out_dir', '--out-dir', dest='out_dir', required=True, help='the game folder to make')
    ap.add_argument('--language', choices=LANGUAGES, help="the game's language (default: the system's)")
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument('--no-shaders', action='store_true', help='skip compiling the shaders ahead (step 6)')
    a = ap.parse_args()
    in_dir, keys, out = (os.path.abspath(os.path.expanduser(p)) for p in (a.in_dir, a.keys, a.out_dir))
    if not os.path.isdir(in_dir):
        sys.exit('--in_dir: %s is not a folder' % in_dir)
    if not os.path.isfile(keys):
        sys.exit('--keys: %s is not a file' % keys)
    if out in (REPO, T.build()) or out.startswith(T.build() + os.sep):
        sys.exit('--out_dir: choose a folder of its own (not the repository or its build folder)')

    print('Super Mario Odyssey %s, recompiled\n  game files: %s\n  keys:       %s\n  game folder: %s'
          % (T.VERSION, in_dir, keys, out))
    print('The folder made contains the game: it is for your own use only; do not share or upload it.')

    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    log = open(LOG, 'a', encoding='utf-8')
    log.write('\n==== %s  %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), ' '.join(sys.argv)))
    print('  progress log: %s\n' % LOG)

    with Task('1/6 Checking tools and libraries') as t:
        code, tail = run_logged([PY, os.path.join(REPO, 'scripts', 'setup.py'), '--only', 'deps'], log, cwd=REPO)
        if code:
            fail(t, 'The check', tail)

    with Task('2/6 Checking hactool') as t:
        hactool = HACTOOL
        if not os.access(hactool, os.X_OK):
            t.finish(ok=False)
            sys.exit('%s is missing or not executable: the repository carries it (git checkout tools/bin/hactool; '
                     'chmod +x it), or rebuild it with tools/build_hactool.py' % os.path.relpath(hactool, REPO))

    state = json.load(open(STATE)) if os.path.exists(STATE) else {}
    key = inputs_key(in_dir, keys)
    with Task('3/6 Decrypting the game files') as t:
        if state.get('dump') == key and os.path.isdir(os.path.join(DUMP, 'romfs')):
            t.finish(note='(done before: same files and keys)')
        else:
            import nsp
            nsp.say = lambda text: (log.write(text + '\n'), log.flush(), t.update(detail=text.strip()))
            stop = threading.Event()

            began = time.time() - 2

            def measure():                         # the bar: what is written so far, against the dump's known size
                while not stop.wait(1.0):          # (only files written now: a dump of an earlier run may still be there)
                    n = 0
                    for root, _, names in os.walk(DUMP):
                        for name in names:
                            try:
                                st = os.lstat(os.path.join(root, name))
                            except OSError:
                                continue
                            if st.st_mtime >= began:
                                n += st.st_size
                    t.update(min(0.99, n / DUMP_BYTES))
            threading.Thread(target=measure, daemon=True).start()
            try:
                nsp.decrypt(in_dir, keys, DUMP, hactool)
            except nsp.SetupError as e:
                stop.set()
                t.finish(ok=False)
                log.write('decrypting failed: %s\n' % e)
                sys.exit('\nDecrypting failed: %s' % e)
            stop.set()
            state['dump'] = key
            os.makedirs(os.path.dirname(STATE), exist_ok=True)
            json.dump(state, open(STATE, 'w'), indent=2)

    print('  4/6 Building the game (the first time: a long while; the next times only what changed)', flush=True)
    cmd = [PY, os.path.join(REPO, 'scripts', 'setup.py'), '--dump', DUMP, '--jobs', str(a.jobs)]
    romfs = T.build('romfs')
    if os.path.lexists(romfs) and os.path.realpath(romfs) != os.path.realpath(os.path.join(DUMP, 'romfs')):
        cmd += ['--redo', 'import']                 # the build was imported from another dump: take this one
    cur = {'task': None, 'name': None}

    def build_line(line):                          # setup.py prints '[<step>] ...' when a step starts, '[<step>] done' when skipped
        m = re.match(r'^\[(\w+)\](.*)$', line)
        if m and m.group(1) == 'deps':             # checked as step 1 already
            return
        if m and m.group(1) in BUILD_STEPS:
            name, rest = m.group(1), m.group(2).strip()
            if name != cur['name']:
                if cur['task']:
                    cur['task'].finish()
                cur['name'] = name
                cur['task'] = Task(BUILD_STEPS[name], indent='      ').__enter__()
            if rest.startswith('done'):
                cur['task'].finish(note='(done before)')
            return
        if cur['task'] and not progress_line(cur['task'], line) and line.strip() and not line.startswith('@'):
            cur['task'].update(detail=line.strip()[:70])
    code, tail = run_logged(cmd, log, build_line, cwd=REPO)
    if cur['task']:
        if code:
            fail(cur['task'], 'Building the game (%s)' % BUILD_STEPS.get(cur['name'], cur['name']), tail)
        cur['task'].finish()
    elif code:
        fail(Task('Building the game'), 'Building the game', tail)

    with Task('5/6 Making the game folder') as t:
        cmd = [PY, os.path.join(REPO, 'tools', 'make_game_folder.py'), '--out', out, '--no-build']
        if a.language:
            cmd += ['--language', a.language]

        def folder_line(line):
            m = re.match(r'^\s*\$ cp .* (\S+)$', line)
            if m:
                t.update(detail='copying ' + os.path.relpath(m.group(1), out) if m.group(1).startswith(out) else line.strip()[:70])
            elif line.strip():
                t.update(detail=line.strip()[:70])
        code, tail = run_logged(cmd, log, folder_line, cwd=REPO)
        if code:
            fail(t, 'Making the game folder', tail)

    with Task('6/6 Compiling shaders') as t:
        if a.no_shaders:
            t.finish(note='(skipped: run %s --compile-shaders later)' % os.path.join(out, 'smo'))
        else:
            t.update(detail='starting (the first time: a few minutes)')
            code, tail = run_logged([os.path.join(out, 'smo'), '--compile-shaders'], log, lambda l: progress_line(t, l))
            if code:
                fail(t, 'Compiling shaders', tail)
    log.close()
    print('\nReady. Play with:\n  %s' % os.path.join(out, 'smo'))


if __name__ == '__main__':
    main()

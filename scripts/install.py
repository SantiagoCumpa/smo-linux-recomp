"""From your own game files to a folder you play from, in one command (the CLI of totk-pc-recomp's setup):

    ./smo-recomp --in_dir <folder with the NSP files> --keys <prod.keys> --out_dir <game folder> [--language es]

    --in_dir    the base game and the 1.4.1 update of Super Mario Odyssey: two NSP files, or one NSP that carries
                both (any names; other files in the folder are ignored)
    --keys      your console's prod.keys
    --out_dir   the game folder to make (created; made again on the same folder, it keeps saves, settings, caches)
    --language  the game's language: ja en-US en-GB fr fr-CA de it es es-419 zh-Hans zh-Hant ko nl ru (default:
                your system's, from LANG; changed later in <out_dir>/smo.env)
    --jobs N    compile jobs (default: all CPUs but two)

Steps, each skipped when its result is already there (a run that stopped resumes):
    1. check     tools and libraries (scripts/setup.py's deps step)
    2. hactool   the NSP decrypter the repository carries (tools/bin/hactool, a static build)
    3. decrypt   the NSP files -> build/install/dump (tools/extract/nsp.py; about 5.5 GB, a minute)
    4. build     lift and compile the game from it (scripts/setup.py; the first time takes long)
    5. folder    the game folder (tools/make_game_folder.py), with the ./smo launcher

No firmware is needed: the game's text uses its own fonts, and the system fonts stand in for the console's shared
fonts. Everything but the game folder stays inside the repository's build/ folder.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'extract'))
import target as T  # noqa: E402

PY = sys.executable
STATE = T.build('install', 'state.json')
DUMP = T.build('install', 'dump')
HACTOOL = os.path.join(REPO, 'tools', 'bin', 'hactool')   # carried by the repository (tools/build_hactool.py rebuilds it)
LANGUAGES = ('ja', 'en-US', 'en-GB', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'zh-Hans', 'zh-Hant', 'ko', 'nl', 'ru')


def step(n, text):
    print('\n[%d/5] %s' % (n, text), flush=True)


def run(cmd, what):
    r = subprocess.run(cmd, cwd=REPO)
    if r.returncode:
        sys.exit('\n%s failed (exit code %d). Fix the cause and run the same command again: it resumes here.'
                 % (what, r.returncode))


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

    step(1, 'checking tools and libraries')
    run([PY, os.path.join(REPO, 'scripts', 'setup.py'), '--only', 'deps'], 'The check')

    step(2, 'checking hactool (decrypts the NSP files)')
    hactool = HACTOOL
    if not os.access(hactool, os.X_OK):
        sys.exit('%s is missing or not executable: the repository carries it (git checkout tools/bin/hactool; '
                 'chmod +x it), or rebuild it with tools/build_hactool.py' % os.path.relpath(hactool, REPO))
    print('  ' + os.path.relpath(hactool, REPO))

    step(3, 'decrypting the game files')
    state = json.load(open(STATE)) if os.path.exists(STATE) else {}
    key = inputs_key(in_dir, keys)
    if state.get('dump') == key and os.path.isdir(os.path.join(DUMP, 'romfs')):
        print('  done before (same files and keys): ' + os.path.relpath(DUMP, REPO))
    else:
        import nsp
        try:
            nsp.decrypt(in_dir, keys, DUMP, hactool)
        except nsp.SetupError as e:
            sys.exit('\nDecrypting failed: %s' % e)
        state['dump'] = key
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        json.dump(state, open(STATE, 'w'), indent=2)

    step(4, 'building the game (the first time: lifting and compiling, a long while)')
    cmd = [PY, os.path.join(REPO, 'scripts', 'setup.py'), '--dump', DUMP, '--jobs', str(a.jobs)]
    romfs = T.build('romfs')
    if os.path.lexists(romfs) and os.path.realpath(romfs) != os.path.realpath(os.path.join(DUMP, 'romfs')):
        cmd += ['--redo', 'import']                 # the build was imported from another dump: take this one
    run(cmd, 'The build')

    step(5, 'making the game folder')
    cmd = [PY, os.path.join(REPO, 'tools', 'make_game_folder.py'), '--out', out, '--no-build']
    if a.language:
        cmd += ['--language', a.language]
    run(cmd, 'Making the game folder')
    print('\nReady. Play with:\n  %s' % os.path.join(out, 'smo'))


if __name__ == '__main__':
    main()

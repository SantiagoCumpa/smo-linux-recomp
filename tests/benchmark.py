"""A benchmark that measures the same thing every time: before and after a change to the runtime.

    python tests/benchmark.py --save <save folder> [--refresh-save] [--stage <stage>] [--resolution 900p] [--runs 2]
                              [--warmup 600] [--measure 1800] [--label "before present thread"]

The save folder is only read. The first time, it is copied to build/bench/save, and every later run uses that copy
(the same place, whatever is played since; --refresh-save copies it again): the game resumes where the save does
(a kingdom's home stage). --stage starts another stage instead (tools/dev/save_warp.py; a kingdom the save has not
reached yet starts with its arrival scene, which waits for input: not usable). Each run starts the game from the
built exe (scripts/run.py, a visible window: presenting to a real window is part of what is measured), with the
repository's default settings (defaults/config/graphics.ini, the resolution given, the default mods), goes from the
title into the game, waits until the scene plays (StageSceneNrvPlay), lets it settle for --warmup frames with Mario
standing still, then measures --measure frames, and ends.

Measured from the log's 300-frame windows that lie inside the measured frames: frames a second (mean) and the 1 %
low, frames over 33 ms; the render thread's milliseconds a frame (batch replay, of it draw preparation; present; wait
for the GPU); draws a frame; and for the whole run: textures made or read at their first use while playing, the peak
resident memory. A run on battery is refused (the log's `power:` line): a laptop then holds its GPU and CPU back.

Prints one line per run and the spread between runs (it should stay within about 5 %), and appends each run to
build/bench/results.jsonl (commit, label, settings, figures) for comparing later.
"""
import argparse
import datetime
import json
import os
import re
import shutil
import statistics
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

PY = sys.executable
OUT = T.build('bench')
RESOLUTIONS = ('720p', '900p', '1080p', '1440p', '2160p')


def git_commit():
    try:
        return subprocess.run(['git', '-C', REPO, 'describe', '--always', '--dirty'], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return '?'


FROZEN = os.path.join(OUT, 'save')


def freeze_save(a):
    """build/bench/save: the save every run starts from (copied once: the benchmark stays the same while the game is played)."""
    if a.refresh_save or not os.path.isdir(FROZEN):
        if os.path.exists(FROZEN):
            shutil.rmtree(FROZEN)
        shutil.copytree(a.save, FROZEN, ignore=shutil.ignore_patterns('save.work.*', 'save.journal.*', 'save.commit'))
        print('benchmark: the save copied to %s (later runs start from this copy; --refresh-save to copy it again)'
              % os.path.relpath(FROZEN, REPO))


def prepare(a, work):
    """The save and the settings of one run, in its own folder."""
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(os.path.join(work, 'config'))
    if a.stage:
        warp = [PY, os.path.join(REPO, 'tools', 'dev', 'save_warp.py'), FROZEN, '--out', os.path.join(work, 'save'),
                '--stage', a.stage, '--visited']
        r = subprocess.run(warp, capture_output=True, text=True)
        if r.returncode:
            sys.exit('save_warp failed:\n' + r.stdout + r.stderr)
    else:
        shutil.copytree(FROZEN, os.path.join(work, 'save'))
    ini = open(os.path.join(REPO, 'defaults', 'config', 'graphics.ini'), encoding='utf-8').read()
    ini = re.sub(r'^resolution = .*$', 'resolution = %s' % a.resolution, ini, flags=re.M)
    ini = re.sub(r'^window = .*$', 'window = windowed', ini, flags=re.M)
    ini = re.sub(r'^performance_overlay = .*$', 'performance_overlay = off', ini, flags=re.M)
    open(os.path.join(work, 'config', 'graphics.ini'), 'w', encoding='utf-8').write(ini)
    return os.path.join(work, 'config', 'graphics.ini')


def parse(log, warmup, measure):
    text = open(log, encoding='utf-8', errors='replace').read()
    res = {}
    if re.search(r'^power: on battery', text, re.M):
        res['battery'] = True
    m = re.search(r'StageSceneNrvPlay seen \(frame (\d+)\)', text)
    if not m:
        return None, 'the scene never played (no StageSceneNrvPlay in the log)'
    play = int(m.group(1))
    lo, hi = play + warmup, play + warmup + measure

    def windows(pattern):
        out = []
        for w in re.finditer(pattern, text, re.M):
            a, b = int(w.group(1)), int(w.group(2))
            if a >= lo and b <= hi:
                out.append(w)
        return out
    fr = windows(r'^nvn_perf: frames (\d+)\.\.(\d+) frame time ms: avg ([\d.]+) min [\d.]+ median ([\d.]+) p99 ([\d.]+) max ([\d.]+) \| fps avg ([\d.]+), 1% low ([\d.]+)')
    vk = windows(r'^nvn_vk_perf: frames (\d+)\.\.(\d+) ms/frame: batch replay ([\d.]+) \(of it: dispatch [\d.]+, draw preparation ([\d.]+).*?wait for the GPU ([\d.]+).*?present ([\d.]+)')
    dr = windows(r'^nvn_vk: frames (\d+)\.\.(\d+): ([\d.]+) fps, (\d+) draws per frame')
    gp = windows(r'^nvn_vk_gpu: frames (\d+)\.\.(\d+): GPU time ([\d.]+) ms per frame')      # SMO_VK_GPUTIME=1
    if not fr or not vk:
        return None, 'no 300-frame window inside the measured frames %d..%d (the run ended early?)' % (lo, hi)
    mean = lambda ws, g: statistics.mean(float(w.group(g)) for w in ws)
    res.update(play_frame=play, windows=len(fr),
               fps=mean(fr, 7), low1=min(float(w.group(8)) for w in fr), p99_ms=max(float(w.group(5)) for w in fr),
               replay_ms=mean(vk, 3), drawprep_ms=mean(vk, 4), gpu_wait_ms=mean(vk, 5), present_ms=mean(vk, 6),
               draws=mean(dr, 4) if dr else 0.0, gpu_ms=mean(gp, 3) if gp else -1.0)
    over = re.findall(r'(\d+) frames over 33\.4 ms', text)
    res['over33'] = int(over[-1]) if over else -1
    t = re.findall(r'made or read at their use: (\d+) times, ([\d.]+) ms \(of them in scene frames: (\d+), ([\d.]+) ms\)', text)
    if t:
        res['tex_at_use_play'] = int(t[-1][2]); res['tex_at_use_play_ms'] = float(t[-1][3])
    mem = [int(x) for x in re.findall(r'^nvn_vk_mem: frame \d+: resident (\d+) MB', text, re.M)]
    res['resident_mb'] = max(mem) if mem else -1
    b = re.search(r'first play frame is frame \d+, ([\d.]+) s after the first present', text)
    res['boot_s'] = float(b.group(1)) if b else -1.0      # (the title screen and the inputs included: the same every run)
    h = re.findall(r'holds of the render thread at a draw: (\d+), (\d+) ms', text)
    res['holds'], res['holds_ms'] = (int(h[-1][0]), int(h[-1][1])) if h else (-1, -1)
    sw = re.findall(r'nvn_vk: swapchain (\d+)x(\d+)', text)
    res['window'] = '%sx%s' % sw[-1] if sw else '?'
    return res, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split('\n\n', 2)[2])
    ap.add_argument('--save', help='a save folder (only read): copied to build/bench/save the first time')
    ap.add_argument('--refresh-save', action='store_true', help='copy --save again')
    ap.add_argument('--stage', help='start this stage instead of where the save resumes (save_warp)')
    ap.add_argument('--resolution', default='900p', choices=RESOLUTIONS)
    ap.add_argument('--runs', type=int, default=2)
    ap.add_argument('--warmup', type=int, default=600, help='frames after the scene plays before measuring (default 10 s)')
    ap.add_argument('--measure', type=int, default=1800, help='frames measured (default 30 s)')
    ap.add_argument('--label', default='', help='a note kept with the results ("before", "present thread" ...)')
    ap.add_argument('--env', action='append', default=[], metavar='NAME=VALUE', help='a runtime switch for the runs (kept with the results)')
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    if a.save:
        a.save = os.path.abspath(os.path.expanduser(a.save))
        freeze_save(a)
    elif not os.path.isdir(FROZEN):
        sys.exit('no build/bench/save yet: give --save <save folder> once')
    commit = git_commit()
    print('benchmark: %s at %s, %d runs of %d frames after %d of settling; commit %s%s'
          % (a.stage or 'where the save resumes', a.resolution, a.runs, a.measure, a.warmup, commit, ' (%s)' % a.label if a.label else ''), flush=True)
    results = []
    for n in range(a.runs):
        work = os.path.join(OUT, 'run')
        cfg = prepare(a, work)
        name = 'bench_%d' % (n + 1)
        seq = '1800:Z 200:Z @nerve:StageSceneNrvPlay %d' % (a.warmup + a.measure + 60)
        cmd = [PY, os.path.join(REPO, 'scripts', 'run.py'), name, '--exe', 'smo_bench', '--seconds', '900',
               '--userdata', work, '--env', 'SMO_SHOW=1', '--env', 'SMO_SEQ=' + seq,      # (the userdata folder: it holds save/)
               '--env', 'SMO_SEQ_EXIT=1', '--env', 'SMO_GRAPHICS_CONFIG=' + cfg,
               '--env', 'SMO_VK_PCACHE=0']                # as the game folder's ./smo runs (--env SMO_VK_PCACHE=1 to compare)
        for kv in a.env:
            cmd += ['--env', kv]
        subprocess.run(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        log = T.build('run', name + '.log')
        res, why = parse(log, a.warmup, a.measure) if os.path.exists(log) else (None, 'no log')
        if not res:
            sys.exit('run %d: %s (log: %s)' % (n + 1, why, log))
        if res.get('battery'):
            sys.exit('run %d: the PC was on battery (the log says so): plug the charger in and run again' % (n + 1))
        results.append(res)
        print('run %d: %5.1f fps (1%% low %4.1f, p99 %5.1f ms, %d frames over 33 ms) | render thread ms/frame: replay %5.2f '
              '(draw prep %5.2f), present %5.2f, GPU wait %5.2f | %4.0f draws | textures at use in play %d (%.0f ms) | '
              'resident %d MB | to play %.1f s, holds %d (%d ms) | window %s%s'
              % (n + 1, res['fps'], res['low1'], res['p99_ms'], res['over33'], res['replay_ms'], res['drawprep_ms'],
                 res['present_ms'], res['gpu_wait_ms'], res['draws'], res.get('tex_at_use_play', -1),
                 res.get('tex_at_use_play_ms', 0), res['resident_mb'], res['boot_s'], res['holds'], res['holds_ms'], res['window'],
                 ' | GPU %.2f ms/frame' % res['gpu_ms'] if res['gpu_ms'] >= 0 else ''), flush=True)
        with open(os.path.join(OUT, 'results.jsonl'), 'a', encoding='utf-8') as f:
            f.write(json.dumps(dict(res, run=n + 1, commit=commit, label=a.label, stage=a.stage, resolution=a.resolution,
                                    warmup=a.warmup, measure=a.measure, env=a.env,
                                    time=datetime.datetime.now().isoformat(timespec='seconds'))) + '\n')
    if len(results) > 1:
        def spread(k):
            v = [r[k] for r in results]
            m = statistics.mean(v)
            return 100.0 * (max(v) - min(v)) / m if m else 0.0
        keys = ('fps', 'replay_ms', 'drawprep_ms', 'present_ms')
        print('spread between runs: ' + ', '.join('%s %.1f%%' % (k, spread(k)) for k in keys)
              + ('' if max(spread(k) for k in keys) <= 5.0 else '  (more than 5 %: something else ran on the PC?)'))
    shutil.rmtree(os.path.join(OUT, 'run'), ignore_errors=True)
    print('results appended to %s' % os.path.relpath(os.path.join(OUT, 'results.jsonl'), REPO))


if __name__ == '__main__':
    main()

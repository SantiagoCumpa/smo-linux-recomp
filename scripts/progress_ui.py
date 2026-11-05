"""The installer's progress display (scripts/install.py): one live line per task, rewritten in place.

    with Task('Translating the game code to C') as t:
        t.update(0.42, 'writing C files 92/220')      # a fraction (0..1) draws a bar; None draws a spinner
    -> '  ✔ Translating the game code to C                      3:12' once done

The tools write everything else to the installer's log; run_logged() runs one of them, keeps its output in the log and
hands each line to a callback that turns '@progress <phase> <done> <total>' lines (SMO_PROGRESS=1) into the bar.
When the output is not a terminal (a pipe, a file), no line is rewritten: a plain line is printed every 10 % or 30 s.
"""
import os
import shutil
import subprocess
import sys
import threading
import time

TTY = sys.stdout.isatty()
OK, FAIL, RUN = ('✔', '✘', '▸') if (sys.stdout.encoding or '').lower().startswith('utf') else ('+', 'x', '>')
SPIN = '|/-\\'


def clock(secs):
    secs = int(secs)
    return '%d:%02d:%02d' % (secs // 3600, secs // 60 % 60, secs % 60) if secs >= 3600 else '%d:%02d' % (secs // 60, secs % 60)


class Task:
    """One line: the task's name, then a bar (or a spinner), a detail and the time it has taken."""
    def __init__(self, label, indent='  '):
        self.label, self.indent = label, indent
        self.frac, self.detail = None, ''
        self.t0 = time.time()
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.last_plain = (-1, 0.0)
        self.thread = None

    def __enter__(self):
        if TTY:
            self.thread = threading.Thread(target=self._loop, daemon=True)
            self.thread.start()
        else:
            print('%s%s %s' % (self.indent, RUN, self.label), flush=True)
        return self

    def __exit__(self, kind, value, tb):
        self.finish(ok=kind is None)
        return False

    def update(self, frac=None, detail=None):
        with self.lock:
            if frac is not None:
                self.frac = max(0.0, min(1.0, frac))
            if detail is not None:
                self.detail = detail
        if not TTY:
            step = int((self.frac or 0) * 10)
            if step != self.last_plain[0] or time.time() - self.last_plain[1] > 30:
                self.last_plain = (step, time.time())
                pct = '%3d%% ' % (self.frac * 100) if self.frac is not None else ''
                print('%s  %s%s' % (self.indent, pct, self.detail), flush=True)

    def _line(self, spin=0):
        width = shutil.get_terminal_size((100, 20)).columns
        with self.lock:
            frac, detail = self.frac, self.detail
        took = clock(time.time() - self.t0)
        head = '%s%s %-34s ' % (self.indent, RUN, self.label)
        if frac is not None:
            n = 24
            full = int(frac * n)
            bar = '[' + '█' * full + '░' * (n - full) + '] %3d%%' % (frac * 100)
        else:
            bar = '[' + SPIN[spin % 4] + ']'
        line = '%s%s  %s' % (head, bar, detail)
        room = width - len(took) - 3
        if len(line) > room:
            line = line[:max(0, room - 1)] + '…'
        return line.ljust(room) + '  ' + took

    def _loop(self):
        k = 0
        while not self.stop.wait(0.2):
            sys.stdout.write('\r' + self._line(k))
            sys.stdout.flush()
            k += 1

    def finish(self, ok=True, note=''):
        if self.stop.is_set():
            return
        self.stop.set()
        if self.thread:
            self.thread.join()
        took = clock(time.time() - self.t0)
        text = '%s%s %-34s %s' % (self.indent, OK if ok else FAIL, self.label, note)
        if TTY:
            width = shutil.get_terminal_size((100, 20)).columns
            sys.stdout.write('\r' + text[:width - len(took) - 3].ljust(width - len(took) - 3) + '  ' + took + '\n')
        else:
            print(text + '  ' + took)
        sys.stdout.flush()


def run_logged(cmd, log, on_line=None, cwd=None, env=None):
    """Run cmd with its output (stdout and stderr) appended to the open file log; each line also goes to on_line. Returns
    the exit code and the last 40 lines (for an error message)."""
    tail = []
    log.write('\n$ %s\n' % ' '.join(cmd))
    log.flush()
    e = dict(os.environ, SMO_PROGRESS='1', PYTHONUNBUFFERED='1')
    e.update(env or {})
    p = subprocess.Popen(cmd, cwd=cwd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors='replace', bufsize=1)
    for line in p.stdout:
        line = line.rstrip('\n')
        log.write(line + '\n')
        tail.append(line)
        del tail[:-40]
        if on_line:
            on_line(line)
    p.wait()
    log.flush()
    return p.returncode, tail

"""Checks scripts/run.py's limit on test games running at once on this PC (take_place / release_place).

    python tests/check_game_places.py

No game is started: threads take places under lock-file names of this check's own in build/run (the real places stay
free). With two places: A and B take them; C and D wait; C gets A's place when A releases it; E gets the place of a
launcher process that is killed while holding it (the kernel drops its flock); smo_native and SMO_MAX_GAMES=0 never
wait. Exit code = number of failed checks."""
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'scripts'))
import run as R  # noqa: E402

NAME = 'check_places_%d_%%d.lock' % os.getpid()
POLL = 0.05
fails = 0


def check(ok, what):
    global fails
    fails += not ok
    print('%s %s' % ('ok  ' if ok else 'FAIL', what))


os.environ['SMO_MAX_GAMES'] = '2'
t0 = time.time()
got = {}
b_may_end = threading.Event()


def holder(tag, hold, release=True):
    h = R.take_place('smo_check', POLL, NAME)
    got[tag] = time.time() - t0
    if hold:
        time.sleep(hold)
    else:
        b_may_end.wait()
    if release:
        R.release_place(h)


def waiter(tag):
    h = R.take_place('smo_check', POLL, NAME)
    got[tag] = time.time() - t0
    time.sleep(0.5)
    R.release_place(h)


a = threading.Thread(target=holder, args=('A', 1.0))
b = threading.Thread(target=holder, args=('B', None))
a.start()
b.start()
time.sleep(0.3)
check('A' in got and 'B' in got and max(got['A'], got['B']) < 0.3, 'A and B take the two places at once (%s)' % got)
h = R.take_place('smo_native', POLL, NAME)
check(h is None, 'smo_native does not wait and takes no place')
c = threading.Thread(target=waiter, args=('C',))
c.start()
time.sleep(0.3)
d = threading.Thread(target=waiter, args=('D',))
d.start()
time.sleep(0.3)
check('C' not in got and 'D' not in got, 'C and D wait while both places are taken')
c.join(5)
d.join(5)
tc, td = got.get('C', -1), got.get('D', -1)
check(min(tc, td) >= 0.95, 'C and D get a place only once A releases its own at 1.0 s (C %.2f s, D %.2f s)' % (tc, td))
check(abs(tc - td) >= 0.45, 'one after the other, as B still holds the other place (C %.2f s, D %.2f s)' % (tc, td))
# abandoned: with three places, this thread holds two, a launcher process the third and is killed; E must get it
b_may_end.set()
b.join(5)
os.environ['SMO_MAX_GAMES'] = '3'
h1 = R.take_place('smo_check', POLL, NAME)
child = subprocess.Popen([sys.executable, '-c', 'import sys, time; sys.path.insert(0, %r); import run as R; '
                          'h = R.take_place("smo_check", %r, %r); print("got", flush=True); time.sleep(60)'
                          % (os.path.dirname(R.__file__), POLL, NAME)], stdout=subprocess.PIPE, text=True,
                         env=dict(os.environ, SMO_MAX_GAMES='3'))
check(child.stdout.readline().strip() == 'got', 'a launcher process takes a second place')
e_got = {}


def e_waiter():
    h = R.take_place('smo_check', POLL, NAME)
    e_got['E'] = time.time()
    R.release_place(h)


h2 = R.take_place('smo_check', POLL, NAME)
e = threading.Thread(target=e_waiter)
e.start()
time.sleep(0.3)
check('E' not in e_got, 'E waits while every place is held')
t_end = time.time()
child.kill()
child.wait()
e.join(5)
check('E' in e_got and e_got['E'] >= t_end, 'E gets the place of the launcher that was killed (the kernel drops its lock)')
R.release_place(h1)
R.release_place(h2)
os.environ['SMO_MAX_GAMES'] = '0'
check(R.take_place('smo_check', POLL, NAME) is None, 'SMO_MAX_GAMES=0: no limit, no place taken')
del os.environ['SMO_MAX_GAMES']
n = R.max_games()
check(n >= 1, 'default limit from the PC\'s memory: %d' % n)
for k in range(3):
    try:
        os.remove(os.path.join(R._place_dir(), NAME % k))
    except OSError:
        pass
print('%d failed' % fails)
sys.exit(fails)

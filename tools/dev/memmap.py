"""Where a running game's memory goes: every mapping of the process with its accessible and resident size, named
where possible.

    python tools/dev/memmap.py <pid | process name> [--log <game log>] [--top 40] [--min-mb 1] [--json <file>] [--zero MB]

Reads the process from outside and writes nothing into it: /proc/<pid>/smaps (mappings, permissions, Rss, shared and
private pages, swap), /proc/<pid>/status (VmRSS, VmHWM, RssAnon/RssFile/RssShmem). Adjacent mappings with the same
name are one "allocation" (a plat_reserve'd range whose committed parts were mprotect'ed splits into several mappings;
a shared library is one per segment). "committed" = the parts that are not PROT_NONE. --log names runtime allocations
whose address the game printed (the memory heap, virtual memory pools, Vulkan memory blocks with SMO_VK_MEMLOG=1).
--zero reads /proc/<pid>/pagemap and /proc/<pid>/mem (needs ptrace access: same user and kernel.yama.ptrace_scope=0,
or root).
"""
import argparse
import bisect
import collections
import json
import os
import re

PAGE = os.sysconf('SC_PAGE_SIZE')


def find_pid(what):
    if what.isdigit():
        return int(what)
    found = []
    for d in os.listdir('/proc'):
        if not d.isdigit():
            continue
        try:
            with open('/proc/%s/comm' % d) as f:
                comm = f.read().strip()
        except OSError:
            continue
        if comm == what or comm == what[:15]:
            found.append(int(d))
    if len(found) != 1:
        raise SystemExit('%d processes named %s' % (len(found), what))
    return found[0]


def smaps(pid):
    """[(start, end, perms, name, {field: bytes})] from /proc/<pid>/smaps."""
    out = []
    head = re.compile(r'^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \d+\s*(.*)$')
    with open('/proc/%d/smaps' % pid) as f:
        for line in f:
            m = head.match(line)
            if m:
                out.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4).strip(), {}))
                continue
            k, _, v = line.partition(':')
            v = v.split()
            if out and len(v) == 2 and v[1] == 'kB':
                out[-1][4][k] = int(v[0]) * 1024
    return out


def status(pid):
    out = {}
    with open('/proc/%d/status' % pid) as f:
        for line in f:
            k, _, v = line.partition(':')
            v = v.split()
            if len(v) == 2 and v[1] == 'kB':
                out[k] = int(v[0]) * 1024
    return out


def threads(pid):
    """{address: label}: the main thread's stack start (other threads' stacks are anonymous mappings)."""
    out = {}
    for tid in os.listdir('/proc/%d/task' % pid):
        try:
            with open('/proc/%d/task/%s/comm' % (pid, tid)) as f:
                comm = f.read().strip()
            with open('/proc/%d/task/%s/stat' % (pid, tid)) as f:
                st = f.read()
        except OSError:
            continue
        fields = st[st.rfind(')') + 2:].split()
        start = int(fields[25])                         # startstack (field 28): meaningful for the main thread
        if start and int(tid) == pid:
            out[start] = 'main thread stack (%s)' % comm
    return out


def log_labels(path):
    """{address: label} from the game's log."""
    out = {}
    if not path:
        return out
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'SetMemoryHeapSize\(0x([0-9a-f]+)\): ok, heap at ([0-9A-Fa-f]+)', line)
        if m:
            out[int(m.group(2), 16)] = 'guest memory heap (memfd view; heap size 0x%s)' % m.group(1)
        m = re.search(r'virtual memory pool (\w+): 0x([0-9a-f]+) bytes at ([0-9a-f]+)', line)
        if m:
            out[int(m.group(3), 16)] = 'NVN virtual memory pool %s (views of the heap memfd)' % m.group(1)
        m = re.search(r'nvn_vk_mem: block (\d+): (\d+) MB, type (\d+) \((.*?)\), class (\d+), mapped at ([0-9A-Fa-f]+)', line)
        if m and int(m.group(6), 16):
            out[int(m.group(6), 16)] = 'Vulkan memory block %s (type %s %s, class %s)' % (m.group(1), m.group(3), m.group(4), m.group(5))
    return out


def kind(name):
    if not name:
        return 'anon'
    if name.startswith('/memfd:') or name.startswith('/dev/'):
        return 'shared'
    if name.startswith('['):
        return name.strip('[]').split(':')[0][:7]
    return 'file'


def zero_pages(pid, A):
    """(pages in memory that hold only zeros, pages not readable, {64 MB band: pages in memory})."""
    zpage = bytes(PAGE)
    zero = unread = 0
    bands = collections.Counter()
    with open('/proc/%d/pagemap' % pid, 'rb') as pm, open('/proc/%d/mem' % pid, 'rb', buffering=0) as mem:
        for s, e in A['ranges']:
            pm.seek((s // PAGE) * 8)
            bits = pm.read(((e - s) // PAGE) * 8)
            for k in range(len(bits) // 8):
                v = int.from_bytes(bits[8 * k:8 * k + 8], 'little')
                if not v >> 63 & 1:                     # not present
                    continue
                va = s + k * PAGE
                bands[(va - A['base']) >> 26] += 1
                try:
                    mem.seek(va)
                    b = mem.read(PAGE)
                except OSError:
                    b = None
                if b is None or len(b) != PAGE:
                    unread += 1
                elif b == zpage:
                    zero += 1
    return zero, unread, bands


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('process')
    ap.add_argument('--log')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--min-mb', type=float, default=1.0)
    ap.add_argument('--json')
    ap.add_argument('--zero', type=float, default=0, metavar='MB',
                    help='for allocations with at least this many MB in memory: how many of those pages hold only zeros, '
                         'and where in the allocation the pages in memory are (64 MB bands); reads only pages in memory')
    a = ap.parse_args()
    pid = find_pid(a.process)
    try:
        maps = smaps(pid)
        st = status(pid)
    except PermissionError:
        raise SystemExit('cannot read /proc/%d/smaps (another user\'s process?)' % pid)
    allocs = collections.OrderedDict()
    last = None
    for s, e, perms, name, f in maps:
        if last and last['end'] == s and last['name'] == name and kind(name) != 'stack':
            A = last
        else:
            A = allocs.setdefault(s, {'base': s, 'end': s, 'type': kind(name), 'reserved': 0, 'committed': 0, 'resident': 0,
                                      'shared': 0, 'swap': 0, 'regions': 0, 'name': name, 'ranges': []})
        A['end'] = e
        A['reserved'] += e - s
        A['regions'] += 1
        A['ranges'].append((s, e))
        if perms[:3] != '---':
            A['committed'] += e - s
        A['resident'] += f.get('Rss', 0)
        A['shared'] += f.get('Shared_Clean', 0) + f.get('Shared_Dirty', 0)
        A['swap'] += f.get('Swap', 0)
        last = A
    starts = sorted(allocs)
    labels = threads(pid)
    labels.update(log_labels(a.log))
    for base, lab in labels.items():
        i = bisect.bisect_right(starts, base) - 1
        if i < 0 or base >= allocs[starts[i]]['end']:
            continue
        A = allocs[starts[i]]
        if base == A['base']:
            A['name'] = lab + (' ' + A['name'] if A['name'] else '')
        elif lab not in A['name']:
            A['name'] = (A['name'] + '; ' if A['name'] else '') + 'contains ' + lab
    mb = lambda x: x / 2 ** 20
    print('process %d: resident %.0f MB (anonymous %.0f MB, file %.0f MB, shared memory %.0f MB), peak resident %.0f MB, '
          'swap %.0f MB, address space %.0f MB'
          % (pid, mb(st.get('VmRSS', 0)), mb(st.get('RssAnon', 0)), mb(st.get('RssFile', 0)), mb(st.get('RssShmem', 0)),
             mb(st.get('VmHWM', 0)), mb(st.get('VmSwap', 0)), mb(st.get('VmSize', 0))))
    tot = collections.Counter()
    for A in allocs.values():
        tot[A['type'] + ' committed'] += A['committed']
        tot[A['type'] + ' resident'] += A['resident']
    print('by type: ' + ', '.join('%s %.0f MB' % (k, mb(v)) for k, v in sorted(tot.items())))
    print('mappings: %d in %d allocations' % (len(maps), len(allocs)))
    rows = sorted(allocs.values(), key=lambda A: -A['resident'])
    print('%-14s %-7s %9s %9s %9s %9s  %s' % ('allocation', 'type', 'reserved', 'committed', 'resident', 'shared', 'name'))
    shown = 0
    rest = collections.Counter()
    for A in rows:
        if shown < a.top and mb(A['resident']) >= a.min_mb:
            print('%014x %-7s %9.1f %9.1f %9.1f %9.1f  %s' % (A['base'], A['type'], mb(A['reserved']), mb(A['committed']),
                                                         mb(A['resident']), mb(A['shared']), A['name'][:150]))
            shown += 1
        else:
            rest['n'] += 1
            rest['committed'] += A['committed']
            rest['resident'] += A['resident']
    print('the other %d allocations: committed %.0f MB, resident %.0f MB' % (rest['n'], mb(rest['committed']), mb(rest['resident'])))
    big = sorted((A for A in allocs.values() if A['committed'] >= 16 << 20), key=lambda A: -A['committed'])
    print('\nallocations with 16 MB or more committed (by commit):')
    for A in big[:a.top]:
        print('%014x %-7s %9.1f %9.1f %9.1f  %s' % (A['base'], A['type'], mb(A['reserved']), mb(A['committed']), mb(A['resident']), A['name'][:150]))
    if a.zero:
        print('\npages in memory that hold only zeros (allocations with %.0f MB or more in memory):' % a.zero)
        for A in rows:
            if mb(A['resident']) < a.zero:
                break
            try:
                zero, unread, bands = zero_pages(pid, A)
            except PermissionError:
                raise SystemExit('cannot read /proc/%d/pagemap or mem (ptrace access: kernel.yama.ptrace_scope)' % pid)
            print('%014x %9.1f MB in memory: %9.1f MB only zeros (%.0f%%)%s  %s' % (
                A['base'], mb(A['resident']), mb(zero * PAGE), 100.0 * zero * PAGE / max(A['resident'], 1),
                ', %d pages not readable' % unread if unread else '', A['name'][:60]))
            if A['reserved'] >= 256 << 20:
                print('    MB in memory per 64 MB band: ' + ' '.join('%d:%.0f' % (k, mb(v * PAGE)) for k, v in sorted(bands.items())))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'pid': pid, 'counters': st, 'allocations': list(allocs.values())}, f)


if __name__ == '__main__':
    main()

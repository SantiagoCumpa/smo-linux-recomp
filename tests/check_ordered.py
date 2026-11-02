"""Barriers and ordered accesses of the lifter (recomp.py), offline: no dump needed.

    python tests/check_ordered.py

1. The C each barrier word becomes: dmb / dsb with the LD option -> acquire fence, with the ST option -> release fence,
   any other option -> sequentially consistent fence; isb -> compiler barrier; clrex -> nothing.
2. LDAR / STLR (8, 16, 32, 64 bits) and LDXR / LDAXR: the lifted C (runtime.h's lda<n> / stl<n> / rd<n>) is compiled with
   clang and run from random register and memory states; registers and memory must equal Unicorn's (the reference
   difftest.py uses). Single-threaded semantics only: the ordering itself is the fence the compiler emits.
recomp.py reads the game's ELF when imported; a minimal ELF with the sections it looks at is written to build/tmp.
Exit status 0 when everything matches.
"""
import os
import random
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = os.path.join(REPO, 'build', 'tmp', 'check_ordered')
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

MEM = 0x10000           # the test's memory window (guest address = host offset into a buffer, see below)


def fake_elf(path):
    """ELF64 with .text, .dynamic (DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0) and .shstrtab: what recomp.py parses."""
    names = b'\0.text\0.dynamic\0.shstrtab\0'
    dyn = b''.join(struct.pack('<qQ', t, 0) for t in (5, 6, 7, 8, 0))
    body = bytearray(0x1200)
    body[0x1100:0x1100 + len(dyn)] = dyn                 # recomp.py reads .dynamic at 0x1000 + its address
    strs = len(body)
    body += names
    shoff = len(body)
    secs = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (1, 1, 6, 0, 0x1000, 0x100, 0, 0, 4, 0),
            (7, 6, 3, 0x100, 0x1100, len(dyn), 0, 0, 8, 16),
            (16, 3, 0, 0, strs, len(names), 0, 0, 1, 0)]
    for s in secs:
        body += struct.pack('<IIQQQQIIQQ', *s)
    body[0:4] = b'\x7fELF'
    struct.pack_into('<Q', body, 0x28, shoff)
    struct.pack_into('<HH', body, 0x3c, len(secs), 3)
    with open(path, 'wb') as f:
        f.write(body)


def main():
    os.makedirs(TMP, exist_ok=True)
    elf = os.path.join(TMP, 'fake.elf')
    fake_elf(elf)
    os.environ['RECOMP_ELF'] = elf
    os.environ['RECOMP_FUNCS'] = os.path.join(TMP, 'none.tsv')
    sys.path.insert(0, os.path.join(REPO, 'tools', 'lifter'))
    import recomp
    fn = recomp.Fn(0x1000, 0x2000, recomp.Env())
    bad = 0

    barriers = {0xd5033bbf: 'SEQ_CST', 0xd5033f9f: 'SEQ_CST', 0xd5033b9f: 'SEQ_CST', 0xd50339bf: 'ACQUIRE',
                0xd50331bf: 'ACQUIRE', 0xd5033abf: 'RELEASE', 0xd5033ebf: 'RELEASE', 0xd5033a9f: 'RELEASE',
                0xd5033fdf: 'signal', 0xd503305f: None}
    for w, want in barriers.items():
        c = fn.lift(w, 0x1000)
        ok = c == ';' if want is None else ('signal_fence' in c if want == 'signal' else
                                            ('thread_fence(__ATOMIC_%s)' % want) in c)
        if not ok:
            print('barrier %08x -> %r, expected %s' % (w, c, want))
            bad += 1
    print('barriers: %d words, %d wrong' % (len(barriers), bad))

    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as A
    XR = [getattr(A, 'UC_ARM64_REG_X%d' % k) for k in range(31)]
    rng = random.Random(1)
    words = []
    for size in range(4):
        base = size << 30
        for rt, rn in ((1, 2), (0, 3), (31, 4), (30, 6)):      # rt != rn: a stored address is a host one here
            r = rt | rn << 5
            words.append(('ldar%d' % (8 << size), base | 0x08dffc00 | r))
            words.append(('stlr%d' % (8 << size), base | 0x089ffc00 | r))
            words.append(('ldaxr%d' % (8 << size), base | 0x085ffc00 | r))
            words.append(('ldxr%d' % (8 << size), base | 0x085f7c00 | r))
    cases = []
    for name, w in words:
        for _ in range(4):
            rn = (w >> 5) & 31
            regs = [rng.getrandbits(64) for _ in range(31)]
            off = rng.randrange(0, 0x100, 8)
            regs[rn] = MEM + off
            mem = bytes(rng.getrandbits(8) for _ in range(0x200))
            cases.append((name, w, regs, mem))
    lines = ['#include <stdio.h>', '#include <stdlib.h>', '#include "runtime.h"',
             'void stx_diag(Ctx *c, uint64_t a, uint64_t e, uint64_t s, uint64_t v, int b) { (void)c; (void)a; (void)e; (void)s; (void)v; (void)b; }']
    for i, (name, w, regs, mem) in enumerate(cases):
        lines.append('static void t%d(Ctx *c) { %s }' % (i, fn.lift(w, 0x1000)))
    lines.append('static void (*tests[])(Ctx *) = {%s};' % ', '.join('t%d' % i for i in range(len(cases))))
    lines.append('''int main(void) {
    static uint8_t buf[0x200];
    uint64_t r[31]; Ctx c;
    for (int i = 0; i < (int)(sizeof tests / sizeof *tests); i++) {
        if (fread(r, 8, 31, stdin) != 31 || fread(buf, 1, sizeof buf, stdin) != sizeof buf) return 2;
        memset(&c, 0, sizeof c);
        for (int k = 0; k < 31; k++) c.x[k] = r[k] >= 0x10000 && r[k] < 0x10200 ? r[k] - 0x10000 + (uint64_t)(uintptr_t)buf : r[k];
        tests[i](&c);
        for (int k = 0; k < 31; k++) { uint64_t v = c.x[k]; if (v >= (uint64_t)(uintptr_t)buf && v < (uint64_t)(uintptr_t)buf + sizeof buf) v = v - (uint64_t)(uintptr_t)buf + 0x10000; fwrite(&v, 8, 1, stdout); }
        fwrite(buf, 1, sizeof buf, stdout);
    }
    return 0;
}''')
    src = os.path.join(TMP, 'ordered.c')
    exe = os.path.join(TMP, 'ordered')
    with open(src, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    r = subprocess.run([T.CLANG, '-O2', '-w', '-ffp-contract=off'] + T.runtime_includes() + [src, '-o', exe, '-lm'],
                       capture_output=True, text=True)
    if r.returncode:
        print((r.stdout + r.stderr)[-3000:])
        sys.exit('check_ordered: build failed')
    inp = b''.join(struct.pack('<31Q', *regs) + mem for _, _, regs, mem in cases)
    out = subprocess.run([exe], input=inp, capture_output=True).stdout
    step = 31 * 8 + 0x200
    if len(out) != step * len(cases):
        sys.exit('check_ordered: the lifted code stopped after %d of %d cases' % (len(out) // step, len(cases)))
    diff = 0
    for i, (name, w, regs, mem) in enumerate(cases):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x1000, 0x1000)
        uc.mem_map(MEM, 0x1000)
        uc.mem_write(0x1000, struct.pack('<I', w))
        uc.mem_write(MEM, mem)
        for k in range(31):
            uc.reg_write(XR[k], regs[k])
        uc.emu_start(0x1000, 0x1004)
        ref = [uc.reg_read(XR[k]) for k in range(31)]
        refmem = bytes(uc.mem_read(MEM, 0x200))
        got = list(struct.unpack_from('<31Q', out, i * step))
        gotmem = out[i * step + 31 * 8:(i + 1) * step]
        if got != ref or gotmem != refmem:
            diff += 1
            if diff <= 5:
                print('%-8s %08x differs: %s' % (name, w, ', '.join('x%d %x != %x' % (k, got[k], ref[k]) for k in range(31)
                                                                     if got[k] != ref[k]) or 'memory'))
    print('ordered and exclusive loads/stores: %d cases (%d words), %d differ from Unicorn' % (len(cases), len(words), diff))
    bad += diff
    print('PASS' if not bad else 'FAIL')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()

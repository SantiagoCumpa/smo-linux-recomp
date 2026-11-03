"""Decrypt Super Mario Odyssey from your NSP files into the dump layout tools/extract/extract.py reads.

    python tools/extract/nsp.py --in-dir <folder with the NSP files> --keys <prod.keys> --out <dump folder>

The folder holds the base game and the 1.4.1 update, as two NSP files or one NSP that carries both (any file names;
other files are ignored). The keys are your console's prod.keys. Writes:

    <out>/exefs/          the update's ExeFS (main, main.npdm, rtld, sdk, subsdk*)
    <out>/romfs/          the base RomFS with the update applied
    <out>/control.nacp    the update's control data
    <out>/manual/         the update's Help pages (HtmlDocument), when present

How: each NSP is a PFS0 archive. Its content meta (the .cnmt.nca, decrypted with hactool) lists the contents of the
base game (an Application) and of the update (a Patch) with their types; the tickets (.tik) give the title keys of
the encrypted contents. hactool (tools/bin/hactool, patched for the update's RomFS layout) extracts the ExeFS
from the update's Program NCA, the RomFS from it with the base Program NCA under it, and the RomFS of the Control
and HtmlDocument NCAs. Work files go to build/install/work (about 6 GB at the peak) and are removed at the end.
Nothing is written outside <out> and build/.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

BASE_ID = 0x0100000000010000                      # Super Mario Odyssey
PATCH_ID = 0x0100000000010800                     # its updates
META_APPLICATION, META_PATCH = 0x80, 0x81
CT_META, CT_PROGRAM, CT_CONTROL, CT_HTML = 0, 1, 3, 4      # content record types of a .cnmt


class SetupError(Exception):
    pass


def say(text):
    print(text, flush=True)


# ---- files: NSP (PFS0) entries, keys, tickets

def pfs0_entries(path):
    """[(name, offset, size)] of an NSP, offsets from the start of the file."""
    with open(path, 'rb') as f:
        head = f.read(16)
        if len(head) < 16 or head[:4] != b'PFS0':
            raise SetupError('%s is not an NSP file (no PFS0 header); XCI files are not supported yet' % path)
        count, strsize = struct.unpack('<II', head[4:12])
        table = f.read(24 * count)
        strings = f.read(strsize)
    base = 16 + 24 * count + strsize
    out = []
    for i in range(count):
        off, size, so, _ = struct.unpack('<QQII', table[24 * i:24 * i + 24])
        out.append((strings[so:strings.index(b'\0', so)].decode('utf-8', 'replace'), base + off, size))
    return out


def read_keys(path):
    """name -> hex string, for the lines hactool accepts (key lengths of whole 16-byte blocks: newer key files carry
    34-digit entries that make this hactool stop)."""
    keys = {}
    try:
        text = open(path, encoding='utf-8', errors='replace').read()
    except OSError as e:
        raise SetupError('cannot read the keys file %s: %s' % (path, e.strerror))
    for line in text.splitlines():
        if '=' not in line or line.lstrip().startswith(('#', ';')):
            continue
        name, value = line.split('=', 1)
        value = re.sub(r'\s', '', value)
        if value and len(value) % 32 == 0 and re.fullmatch(r'[0-9a-fA-F]+', value):
            keys[name.strip().lower()] = value
    if 'header_key' not in keys:
        raise SetupError('%s has no header_key: is it a prod.keys file?' % path)
    return keys


def copy_range(src, offset, size, dst):
    with open(src, 'rb') as fi, open(dst, 'wb') as fo:
        left, pos = size, offset
        while left:
            n = os.copy_file_range(fi.fileno(), fo.fileno(), min(left, 1 << 30), pos)
            if n <= 0:
                raise SetupError('short read in %s at %d' % (src, pos))
            pos += n
            left -= n


def nca_header(nsp, offset, header_key):
    """(content type, title id, rights id hex) from an NCA's header (AES-XTS, 0x200-byte sectors numbered big-endian)."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    with open(nsp, 'rb') as f:
        f.seek(offset)
        raw = f.read(0x400)
    key = bytes.fromhex(header_key)
    plain = b''
    for s in range(2):
        d = Cipher(algorithms.AES(key), modes.XTS(s.to_bytes(16, 'big'))).decryptor()
        plain += d.update(raw[s * 0x200:(s + 1) * 0x200]) + d.finalize()
    if plain[0x200:0x204] != b'NCA3':
        raise SetupError('an NCA of %s does not decrypt with header_key: wrong or old prod.keys?' % os.path.basename(nsp))
    return plain[0x205], struct.unpack('<Q', plain[0x210:0x218])[0], plain[0x230:0x240].hex()


# ---- hactool

class Hactool:
    def __init__(self, exe, keys, work):
        self.exe, self.work = exe, work
        self.keyfile = os.path.join(work, 'prod.keys')
        self.home = os.path.join(work, 'home')
        os.makedirs(os.path.join(self.home, '.switch'), exist_ok=True)
        with open(self.keyfile, 'w') as f:
            f.writelines('%s = %s\n' % kv for kv in keys.items())
        os.chmod(self.keyfile, 0o600)

    def title_keys(self, tickets):
        """rights id -> encrypted title key, in hactool's title.keys (it decrypts them with the titlekek)."""
        path = os.path.join(self.home, '.switch', 'title.keys')
        with open(path, 'w') as f:
            f.writelines('%s = %s\n' % kv for kv in tickets.items())
        os.chmod(path, 0o600)

    def run(self, args, what):
        env = dict(os.environ, HOME=self.home)
        r = subprocess.run([self.exe, '-k', self.keyfile, '--disablekeywarns', '--suppresskeys'] + args, env=env,
                           capture_output=True, text=True, errors='replace')
        out = r.stdout + r.stderr
        bad = [l for l in out.splitlines() if re.search(r'Failed|Invalid|Error|Unable|not found|corrupt', l, re.I)]
        if r.returncode or bad:
            raise SetupError('%s failed (hactool):\n  %s' % (what, '\n  '.join(bad[-8:] or out.splitlines()[-8:])))
        return out


def parse_cnmt(path):
    """(title id, meta type, [(content type, nca id hex, size)]) of a .cnmt file."""
    data = open(path, 'rb').read()
    tid, = struct.unpack('<Q', data[0:8])
    mtype = data[0xC]
    table, count = struct.unpack('<HH', data[0xE:0x12])
    rec = []
    for i in range(count):
        r = data[0x20 + table + 0x38 * i:0x20 + table + 0x38 * (i + 1)]
        rec.append((r[0x36], r[0x20:0x30].hex(), int.from_bytes(r[0x30:0x36], 'little')))
    return tid, mtype, rec


# ---- the whole decryption

def decrypt(in_dir, keys_path, out, hactool_exe, work=None):
    work = work or T.build('install', 'work')
    if not os.path.isdir(in_dir):
        raise SetupError('the input folder %s does not exist' % in_dir)
    nsps = sorted(os.path.join(in_dir, n) for n in os.listdir(in_dir) if n.lower().endswith('.nsp'))
    if not nsps:
        raise SetupError('no .nsp file in %s (the base game and the 1.4.1 update)' % in_dir)
    keys = read_keys(keys_path)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    ht = Hactool(hactool_exe, keys, work)
    try:
        # every entry of every NSP: NCAs by id, tickets
        ncas, tickets = {}, {}
        for nsp in nsps:
            say('  reading %s' % os.path.basename(nsp))
            for name, off, size in pfs0_entries(nsp):
                low = name.lower()
                if low.endswith('.nca'):
                    ncas[low.split('.')[0]] = (nsp, off, size, low.endswith('.cnmt.nca'))
                elif low.endswith('.tik'):
                    with open(nsp, 'rb') as f:
                        f.seek(off)
                        t = f.read(size)
                    rights = t[0x2a0:0x2b0].hex()
                    if t[0x281] != 0:
                        raise SetupError('the ticket %s is personalized (bound to one console): dump the game again with '
                                         'a common ticket (nxdumptool: "remove console specific data")' % name)
                    tickets[rights] = t[0x180:0x190].hex()
        ht.title_keys(tickets)

        # the content meta of each NSP: which NCA is what, for the base game and the update
        contents = {}                              # (meta type, content type) -> (nca id, size)
        for nid, (nsp, off, size, is_meta) in ncas.items():
            if not is_meta:
                continue
            path = os.path.join(work, nid + '.cnmt.nca')
            copy_range(nsp, off, size, path)
            mdir = os.path.join(work, 'meta_' + nid)
            ht.run(['-t', 'nca', '--section0dir=' + mdir, path], 'reading the content meta of ' + os.path.basename(nsp))
            for cnmt in os.listdir(mdir):
                if cnmt.endswith('.cnmt'):
                    tid, mtype, rec = parse_cnmt(os.path.join(mdir, cnmt))
                    if (tid, mtype) not in ((BASE_ID, META_APPLICATION), (PATCH_ID, META_PATCH)):
                        continue
                    for ctype, cid, csize in rec:
                        if ctype in (CT_PROGRAM, CT_CONTROL, CT_HTML):
                            contents[(mtype, ctype)] = (cid, csize)
        if (META_APPLICATION, CT_PROGRAM) not in contents:
            raise SetupError('the base game of Super Mario Odyssey (title 0100000000010000) is not in %s' % in_dir)
        if (META_PATCH, CT_PROGRAM) not in contents:
            raise SetupError('the update of Super Mario Odyssey (1.4.1, title 0100000000010800) is not in %s' % in_dir)

        def nca(mtype, ctype, label):
            cid, _ = contents[(mtype, ctype)]
            if cid not in ncas:
                raise SetupError('the %s (NCA %s) is listed but missing from the NSP files' % (label, cid))
            nsp, off, size, _ = ncas[cid]
            ctype_h, _, rights = nca_header(nsp, off, keys['header_key'])
            if rights != '0' * 32 and rights not in tickets:
                raise SetupError('no ticket for the %s (rights id %s) in the NSP files' % (label, rights))
            if ctype_h not in (0, 2, 3):
                raise SetupError('the %s (NCA %s) is not a program, control or manual content' % (label, cid))
            path = os.path.join(work, cid + '.nca')
            if not os.path.exists(path):
                say('  copying the %s (%d MB)' % (label, size >> 20))
                copy_range(nsp, off, size, path)
            return path

        base_prog = nca(META_APPLICATION, CT_PROGRAM, 'base game program')
        upd_prog = nca(META_PATCH, CT_PROGRAM, 'update program')
        upd_ctl = nca(META_PATCH, CT_CONTROL, 'update control data')

        if os.path.exists(out):
            shutil.rmtree(out)
        os.makedirs(out)
        say('  decrypting the ExeFS')
        ht.run(['-t', 'nca', '--exefsdir=' + os.path.join(out, 'exefs'), upd_prog], 'extracting the ExeFS')
        say('  decrypting the RomFS with the update applied (a few minutes)')
        ht.run(['-t', 'nca', '--basenca=' + base_prog, '--romfsdir=' + os.path.join(out, 'romfs'), upd_prog],
               'extracting the RomFS')
        os.remove(base_prog)
        os.remove(upd_prog)
        cdir = os.path.join(work, 'control')
        ht.run(['-t', 'nca', '--romfsdir=' + cdir, upd_ctl], 'extracting the control data')
        shutil.copy2(os.path.join(cdir, 'control.nacp'), os.path.join(out, 'control.nacp'))
        if (META_PATCH, CT_HTML) in contents or (META_APPLICATION, CT_HTML) in contents:
            # the update's Help pages are a patch (BKTR) on the base game's, as its RomFS is
            args = ['-t', 'nca', '--romfsdir=' + os.path.join(out, 'manual')]
            if (META_PATCH, CT_HTML) in contents and (META_APPLICATION, CT_HTML) in contents:
                args += ['--basenca=' + nca(META_APPLICATION, CT_HTML, 'base Help pages'), nca(META_PATCH, CT_HTML, 'Help pages')]
            else:
                args += [nca(META_PATCH, CT_HTML, 'Help pages') if (META_PATCH, CT_HTML) in contents else
                         nca(META_APPLICATION, CT_HTML, 'Help pages')]
            say('  decrypting the Help pages')
            ht.run(args, 'extracting the Help pages')
    finally:
        shutil.rmtree(work, ignore_errors=True)     # NCAs, the key copies
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--in-dir', '--in_dir', dest='in_dir', required=True, help='folder with the NSP files')
    ap.add_argument('--keys', required=True, help='your prod.keys')
    ap.add_argument('--out', required=True, help='the dump folder to write')
    a = ap.parse_args()
    try:
        decrypt(os.path.abspath(a.in_dir), os.path.abspath(a.keys), os.path.abspath(a.out),
                os.path.join(REPO, 'tools', 'bin', 'hactool'))
    except SetupError as e:
        sys.exit('nsp: ' + str(e))
    print('dump: ' + os.path.abspath(a.out))


if __name__ == '__main__':
    main()

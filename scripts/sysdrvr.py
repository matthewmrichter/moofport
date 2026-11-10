#!/usr/bin/env python3
"""sysdrvr.py - install MoofPort as the System file's DRVR 47 ".ENET0" on a disk image, or undo it.

    scripts/sysdrvr.py install   <image.hda> [build-mac/MoofPort.bin]
    scripts/sysdrvr.py uninstall <image.hda>
    scripts/sysdrvr.py show      <image.hda>

Why (docs/dev/PLAN.md decision 3, docs/dev/BOOT-CRASH-ETHERTALK.md): with EtherTalk selected, AppleTalk's
INIT in the System file opens ".ENET0" before any System Folder INIT runs, so the MoofPort INIT
comes too late. As DRVR 47 in the System file MoofPort is the driver AppleTalk opens at boot.

install:   DaynaPORT's DRVR 47 is renamed reversibly to type 'DRVx' (same id, name, attributes
           and bytes), and MoofPort's DRVR 128 from MoofPort.bin is added as DRVR 47 ".ENET0".
uninstall: removes our DRVR 47 and gives DRVx 47 back its type.
The whole resource fork is parsed and written back. Only images under $MP_HOME are accepted
(never MoofTCP's golden.hda, never the real-Classic originals), and Snow must not be running. The
image's HFS partition is found from the Apple partition map. hfsutils runs with its own HOME, since
~/.hcwd is shared with MoofTCP. A Mac-side installer app comes later (PLAN Phase 2).
"""
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP_HOME = os.environ.get("MP_HOME", os.path.expanduser("~/.local/share/moofport"))
SYSTEM = ":System Folder:System"


def die(msg):
    sys.exit("sysdrvr.py: " + msg)


# ---- resource forks (Inside Macintosh: More Macintosh Toolbox, Resource Manager format) ----

def parse_fork(f):
    """[(type, id, name or None, attrs, data)] in map order, and the map's file attributes."""
    if not f:
        return [], 0
    doff, moff, dlen, mlen = struct.unpack(">IIII", f[:16])
    m = f[moff:moff + mlen]
    fattr, tloff, nloff = struct.unpack(">HHH", m[22:28])
    ntypes = struct.unpack(">H", m[tloff:tloff + 2])[0] + 1
    out = []
    for i in range(ntypes):
        t, cnt, roff = struct.unpack(">4sHH", m[tloff + 2 + 8 * i:tloff + 10 + 8 * i])
        for j in range(cnt + 1):
            rid, noff, a_d = struct.unpack(">hHI", m[tloff + roff + 12 * j:tloff + roff + 12 * j + 8])
            attrs, dpos = a_d >> 24, a_d & 0xFFFFFF
            name = None
            if noff != 0xFFFF:
                n = m[nloff + noff]
                name = m[nloff + noff + 1:nloff + noff + 1 + n]
            ln = struct.unpack(">I", f[doff + dpos:doff + dpos + 4])[0]
            out.append((t, rid, name, attrs, f[doff + dpos + 4:doff + dpos + 4 + ln]))
    return out, fattr


def build_fork(res, fattr):
    types = []
    for r in res:
        if r[0] not in types:
            types.append(r[0])
    data, names = bytearray(), bytearray()
    refs = {t: [] for t in types}
    for t, rid, name, attrs, body in res:
        noff = 0xFFFF
        if name is not None:
            noff = len(names)
            names += bytes([len(name)]) + name
        refs[t].append(struct.pack(">hHII", rid, noff, (attrs << 24) | len(data), 0))
        data += struct.pack(">I", len(body)) + body
    tl = bytearray(struct.pack(">H", len(types) - 1))
    rl = bytearray()
    roff0 = 2 + 8 * len(types)
    for t in types:
        tl += struct.pack(">4sHH", t, len(refs[t]) - 1, roff0 + len(rl))
        rl += b"".join(refs[t])
    tloff = 28
    nloff = tloff + len(tl) + len(rl)
    doff = 256
    moff = doff + len(data)
    mlen = nloff + len(names)
    hdr = struct.pack(">IIII", doff, moff, len(data), mlen)
    m = hdr + struct.pack(">IHHHH", 0, 0, fattr, tloff, nloff) + tl + rl + names
    return hdr + bytes(240) + bytes(data) + m


# ---- MacBinary ----

def crc16x(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def mb_parse(blob):
    dl, rl = struct.unpack(">II", blob[83:91])
    ro = 128 + (dl + 127) // 128 * 128
    return blob[:128], blob[128:128 + dl], blob[ro:ro + rl]


def mb_build(hdr, data, rsrc):
    h = bytearray(hdr)
    h[83:91] = struct.pack(">II", len(data), len(rsrc))
    h[124:126] = struct.pack(">H", crc16x(bytes(h[:124])))
    pad = lambda b: b + b"\0" * (-len(b) % 128)
    return bytes(h) + pad(data) + pad(rsrc)


# ---- the image ----

def hfs_start(img):
    """Byte offset of the first Apple_HFS partition."""
    d = open(img, "rb").read(512 * 64)
    for i in range(1, 64):
        b = d[512 * i:512 * i + 512]
        if b[:2] != b"PM":
            break
        if b[48:80].split(b"\0")[0] == b"Apple_HFS":
            return struct.unpack(">I", b[8:12])[0] * 512
    die("no Apple_HFS partition in " + img)


def clear_unmounted_bit(img):
    """hfsutils sets the MDB's "cleanly unmounted" attribute (drAtrb 0x0100) when it unmounts.
    On the real-Classic image Snow then hangs at "Welcome to Macintosh" on the first boot (an
    illegal-instruction loop, docs/dev/BOOT-CRASH-ETHERTALK.md "Pitfall"); a Mac leaves it clear
    while mounted. Put it back the way the Mac had it: byte at partition + 1024 + 10, &= 0xFE."""
    off = hfs_start(img) + 1024 + 10
    with open(img, "r+b") as f:
        f.seek(off)
        b = f.read(1)[0]
        f.seek(off)
        f.write(bytes([b & 0xFE]))


def hfs_partition(img):
    d = open(img, "rb").read(512 * 64)
    n = 0
    for i in range(1, 64):
        b = d[512 * i:512 * i + 512]
        if b[:2] != b"PM":
            break
        if b[48:80].split(b"\0")[0] == b"Apple_HFS":
            n += 1
            return str(n)
    die("no Apple_HFS partition in " + img)


class Volume:
    def __init__(self, img):
        self.tmp = tempfile.mkdtemp(prefix="sysdrvr-")
        self.env = dict(os.environ, HOME=self.tmp)
        self.img = img

    def run(self, *cmd):
        r = subprocess.run(cmd, env=self.env, capture_output=True)
        if r.returncode:
            die("%s failed: %s" % (cmd[0], r.stderr.decode(errors="replace").strip()))
        return r.stdout

    def __enter__(self):
        self.run("hmount", self.img, hfs_partition(self.img))
        return self

    def __exit__(self, *a):
        subprocess.run(["humount"], env=self.env, capture_output=True)
        clear_unmounted_bit(self.img)


def check_image(img):
    img = os.path.realpath(img)
    if not img.startswith(os.path.realpath(MP_HOME) + os.sep):
        die("only images under %s (a copy, never an original): %s" % (MP_HOME, img))
    if subprocess.run([os.path.join(ROOT, "scripts", "snow.sh"), "status"],
                      capture_output=True).returncode == 0:
        die("Snow is running; stop it first")
    return img


def find(res, t, rid):
    return [i for i, r in enumerate(res) if r[0] == t and r[1] == rid]


def show(res):
    for t, rid, name, attrs, body in res:
        if t in (b"DRVR", b"DRVx") and rid in (47, 127, 128):
            print("  %s %d %r attrs 0x%02x, %d bytes" % (t.decode(), rid, name, attrs, len(body)))


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("install", "uninstall", "show"):
        die("usage: sysdrvr.py install|uninstall|show <image.hda> [MoofPort.bin]")
    op, img = sys.argv[1], check_image(sys.argv[2])
    with Volume(img) as v:
        sysbin = os.path.join(v.tmp, "System.bin")
        v.run("hcopy", "-m", SYSTEM, sysbin)
        hdr, data, rsrc = mb_parse(open(sysbin, "rb").read())
        res, fattr = parse_fork(rsrc)
        if op == "show":
            show(res)
            return
        mine, dayna, parked = find(res, b"DRVR", 47), [], find(res, b"DRVx", 47)
        if op == "install":
            if parked:
                die("already installed (DRVx 47 exists); uninstall first")
            if len(mine) != 1:
                die("expected one DRVR 47 (DaynaPORT) in the System file, found %d" % len(mine))
            t, rid, name, attrs, body = res[mine[0]]
            if name != b".ENET0":
                die("DRVR 47 is %r, not DaynaPORT's .ENET0" % name)
            res[mine[0]] = (b"DRVx", rid, name, attrs, body)
            src = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, "build-mac", "MoofPort.bin")
            ours = [r for r in parse_fork(mb_parse(open(src, "rb").read())[2])[0]
                    if r[0] == b"DRVR" and r[1] == 128]
            if len(ours) != 1:
                die("no DRVR 128 in " + src)
            # sysheap (0x40) | locked (0x10): loaded into the system heap and never moved
            res.append((b"DRVR", 47, b".ENET0", 0x50, ours[0][4]))
        else:
            if len(parked) != 1:
                die("not installed (no DRVx 47)")
            for i in sorted(mine, reverse=True):
                del res[i]
            parked = find(res, b"DRVx", 47)
            t, rid, name, attrs, body = res[parked[0]]
            res[parked[0]] = (b"DRVR", rid, name, attrs, body)
        new = build_fork(res, fattr)
        key = lambda r: (r[0], r[1])
        if sorted(parse_fork(new)[0], key=key) != sorted(res, key=key):
            die("resource fork round trip failed; image not changed")
        out = os.path.join(v.tmp, "System.new.bin")
        open(out, "wb").write(mb_build(hdr, data, new))
        v.run("hdel", SYSTEM)
        v.run("hcopy", "-m", out, SYSTEM)
        v.run("hcopy", "-m", SYSTEM, sysbin)
        back = parse_fork(mb_parse(open(sysbin, "rb").read())[2])[0]
        if sorted(back, key=key) != sorted(res, key=key):
            die("the System file read back differs from what was written")
        print("%s: %s done" % (img, op))
        show(back)


if __name__ == "__main__":
    main()

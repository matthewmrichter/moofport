#!/usr/bin/env python3
"""Independent verification of the release artifacts (helper of scripts/release-verify.sh).

    relverify.py [--self-test] [--dist DIR] <version>

Port of MoofTCP's scripts/relverify.py for MoofPort's payload (docs/dev/RELEASE.md). Reads
dist/MoofPort-<version>.{dsk,sit,sit.hqx,sha256} with readers that did not write them:
  .sit.hqx  macutils `hexbin` (must give back one file "MoofPort-<v>.sit", SIT!/SIT!, no resource
            fork, whose data fork is byte for byte the .sit beside it; that decoded .sit is then
            read again with unar), and `unar`/`lsar` on the .sit.hqx itself (it looks through the
            BinHex into the StuffIt archive)
  .sit      `unar`/`lsar`
  .dsk      hfsutils `hmount`/`hls -a`/`hcopy -m` (819200 bytes, volume "MoofPort", exactly the
            four files at the root)
  .sha256   recomputed with hashlib: exactly one line per artifact, each matching
and compares every file found with what it must be:
  MoofPortInstall, MoofStat, MoofTune: build-mac/<name>.bin (name, type APPL, creators MPIN/MPST/MPTU,
            Finder flags with hasBundle, both fork lengths, sha256 of both forks);
  Read Me:  derived here from docs/release/ReadMe.txt (LF -> CR, ttro/ttxt, flags 0, no resource
            fork), not taken from release.sh's output.
Finder flags: all 16 bits except 0x0100 (kHasBeenInited, which the Finder sets by itself). Dates are
not compared. It also runs scripts/relvers.py's checks on build-mac/MoofPortInstall.bin (vers, log
line, no diagnostic build, 'MPDV' 128 = build-mac/MoofPortDrv.flt).
hfsutils keeps its current volume in $HOME/.hcwd, so every hfsutils call here runs with HOME set to
a private temporary directory (a MoofTCP or deploy.sh job may be using its own at the same time).

--self-test first requires the real artifacts to PASS, then makes broken copies and requires each
one to FAIL: flipped resource-fork bytes (.sit, .dsk), a changed BinHex character, a changed type
code in the .dsk (hattrib), CRC-valid wrong archives (a creator XXXX, a cleared bundle bit, a
missing Read Me, a Read Me with one changed word, the apps of another version in a valid .sit.hqx),
a truncated .dsk, an extra invisible file in the .dsk, a wrong checksum line, and an installer
whose vers or driver differs (relvers). Exit 0 only if all of that holds.
"""
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLAG_MASK = 0xFEFF
VOLNAME = "MoofPort"
DSK_SIZE = 819200
APPS = (("MoofPortInstall", "MPIN"), ("MoofStat", "MPST"), ("MoofTune", "MPTU"))
README_SRC = os.path.join(ROOT, "docs", "release", "ReadMe.txt")
DRV_FLT = os.path.join(ROOT, "build-mac", "MoofPortDrv.flt")
MAC_EPOCH = 2082844800

_spec = importlib.util.spec_from_file_location("relvers", os.path.join(ROOT, "scripts", "relvers.py"))
relvers = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(relvers)


class Fail(Exception):
    pass


HFS_HOME = tempfile.mkdtemp(prefix="relv-hcwd-")
HFS_ENV = dict(os.environ, HOME=HFS_HOME)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def rec(name, typ, creator, flags, data, rsrc):
    return dict(name=name, type=typ, creator=creator, flags=flags & FLAG_MASK,
                dlen=len(data), rlen=len(rsrc), dsha=sha(data), rsha=sha(rsrc))


def parse_mb(blob, what):
    try:
        return relvers.macbinary(blob, what)
    except relvers.Bad as e:
        raise Fail(str(e))


def rec_mb(blob, what):
    return rec(*parse_mb(blob, what))


def crc16x(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def fix_crc(h):
    h[124:126] = struct.pack(">H", crc16x(bytes(h[:124])))


def macbinary(name, typ, creator, data, rsrc=b"", flags=0):
    """A MacBinary II file (the same layout release.sh writes)."""
    now = int(time.time()) + MAC_EPOCH
    h = bytearray(128)
    nb = name.encode("mac_roman")
    h[1] = len(nb)
    h[2:2 + len(nb)] = nb
    h[65:69] = typ
    h[69:73] = creator
    h[73] = flags >> 8
    h[101] = flags & 0xFF
    h[83:91] = struct.pack(">II", len(data), len(rsrc))
    h[91:99] = struct.pack(">II", now, now)
    h[122] = h[123] = 129
    fix_crc(h)
    pad = lambda b: b + b"\0" * (-len(b) % 128)
    return bytes(h) + pad(data) + pad(rsrc)


def readme_text(txt=None):
    """The Read Me data fork: docs/release/ReadMe.txt (or the given bytes), checked, LF -> CR."""
    if txt is None:
        txt = open(README_SRC, "rb").read()
    if any(b > 126 or (b < 32 and b not in (10, 13)) for b in txt):
        raise Fail("ReadMe.txt must be plain ASCII")
    if not txt.endswith(b"\n"):
        txt += b"\n"
    return txt.replace(b"\r\n", b"\n").replace(b"\n", b"\r")


def app_bin(name):
    return os.path.join(ROOT, "build-mac", name + ".bin")


def expected():
    """The four files every archive must hold, in archive order."""
    want = []
    for name, creator in APPS:
        p = app_bin(name)
        if not os.path.isfile(p):
            raise Fail("missing reference " + p)
        r = rec_mb(open(p, "rb").read(), p)
        if (r["name"], r["type"], r["creator"]) != (name, "APPL", creator) or not r["flags"] & 0x2000:
            raise Fail("reference %s has identity %s/%s/%s flags %04x, want %s/APPL/%s with hasBundle"
                       % (p, r["name"], r["type"], r["creator"], r["flags"], name, creator))
        want.append(r)
    want.append(rec("Read Me", "ttro", "ttxt", 0, readme_text(), b""))
    return want


def run(cmd, env=None, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, **kw)
    if r.returncode != 0:
        raise Fail("%s failed (%d): %s" % (cmd[0], r.returncode, r.stderr.decode("latin-1", "replace").strip()[:300]))
    return r.stdout


def hfs(cmd):
    return run(cmd, env=HFS_ENV)


def humount():
    subprocess.run(["humount"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=HFS_ENV)


def cc(v):
    return struct.pack(">I", v).decode("mac_roman")


def appledouble_rsrc(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x00\x05\x16\x07":
        raise Fail("%s: not AppleDouble" % path)
    n = struct.unpack(">H", d[24:26])[0]
    for i in range(n):
        eid, off, ln = struct.unpack(">III", d[26 + 12 * i:38 + 12 * i])
        if eid == 2:
            return d[off:off + ln]
    return b""


def read_unar(archive):
    """Entries as lsar lists them and unar extracts them (folders count as unexpected entries)."""
    meta = json.loads(run(["lsar", "-j", archive]), strict=False)["lsarContents"]
    out = {}
    for e in meta:
        nm = e["XADFileName"]
        if e.get("XADIsDirectory"):
            out[nm + "/"] = None
            continue
        o = out.setdefault(nm, dict(type=None, creator=None, flags=0, need_data=False, need_rsrc=False))
        if e.get("XADFileSize", 0) > 0:
            o["need_rsrc" if e.get("XADIsResourceFork") else "need_data"] = True
        if "XADFileType" in e:
            o["type"], o["creator"] = cc(e["XADFileType"]), cc(e["XADFileCreator"])
            o["flags"] |= e.get("XADFinderFlags", 0)
    tmp = tempfile.mkdtemp(prefix="relv-unar-")
    try:
        run(["unar", "-q", "-D", "-k", "visible", "-o", tmp, archive])
        res = []
        for nm, o in out.items():
            if o is None:
                res.append(rec(nm, "fldr", "", 0, b"", b""))
                continue
            p = os.path.join(tmp, nm)
            rp = p + ".rsrc"
            if o["need_data"] and not os.path.isfile(p):
                raise Fail("unar listed a data fork for %r but extracted no file" % nm)
            if o["need_rsrc"] and not os.path.isfile(rp):
                raise Fail("unar listed a resource fork for %r but extracted no .rsrc file" % nm)
            data = open(p, "rb").read() if os.path.isfile(p) else b""
            rsrc = appledouble_rsrc(rp) if os.path.isfile(rp) else b""
            res.append(rec(nm, o["type"], o["creator"], o["flags"], data, rsrc))
        extra = [f for f in os.listdir(tmp) if f not in out and not (f.endswith(".rsrc") and f[:-5] in out)]
        if extra:
            raise Fail("unar produced unexpected files: %r" % extra)
        return res
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def read_hexbin(hqx, sit_path, ver):
    """hexbin -> exactly one SIT!/SIT! file equal to the .sit; then that decoded .sit through unar."""
    tmp = tempfile.mkdtemp(prefix="relv-hexbin-")
    try:
        run(["hexbin", os.path.abspath(hqx)], cwd=tmp)
        files = os.listdir(tmp)
        if len(files) != 1:
            raise Fail("hexbin produced %r" % files)
        name, typ, cre, _fl, data, rsrc = parse_mb(open(os.path.join(tmp, files[0]), "rb").read(), "hexbin output")
        want_name = "MoofPort-%s.sit" % ver
        if (name, typ, cre) != (want_name, "SIT!", "SIT!") or rsrc:
            raise Fail("hexbin gave %r %s/%s rsrc %d, want %r SIT!/SIT! with no resource fork"
                       % (name, typ, cre, len(rsrc), want_name))
        if not os.path.isfile(sit_path) or data != open(sit_path, "rb").read():
            raise Fail("the decoded .sit is not byte for byte %s" % os.path.basename(sit_path))
        dec = os.path.join(tmp, "decoded.sit")
        open(dec, "wb").write(data)
        return read_unar(dec)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def read_dsk(dsk):
    if os.path.getsize(dsk) != DSK_SIZE:
        raise Fail("%s is %d bytes, not %d" % (os.path.basename(dsk), os.path.getsize(dsk), DSK_SIZE))
    tmp = tempfile.mkdtemp(prefix="relv-hfs-")
    mounted = False
    try:
        out = hfs(["hmount", os.path.abspath(dsk)]).decode("mac_roman")
        mounted = True
        if 'Volume name is "%s"' % VOLNAME not in out:
            raise Fail("volume name is not %r: %s" % (VOLNAME, out.splitlines()[:1]))
        names = [n for n in hfs(["hls", "-a", "-1", ":"]).decode("mac_roman").split("\n") if n]
        res = []
        for i, nm in enumerate(names):
            p = os.path.join(tmp, "f%d.bin" % i)
            hfs(["hcopy", "-m", ":" + nm, p])
            r = rec_mb(open(p, "rb").read(), nm)
            if r["name"] != nm:
                raise Fail("hcopy name %r != listing %r" % (r["name"], nm))
            res.append(r)
        return res
    finally:
        if mounted:
            humount()
        shutil.rmtree(tmp, ignore_errors=True)


def check_sums(path, paths):
    lines = [l for l in open(path, "rb").read().decode("ascii").split("\n") if l]
    want = {os.path.basename(paths[k]): paths[k] for k in ("dsk", "hqx", "sit")}
    seen = {}
    for l in lines:
        h, sep, name = l.partition("  ")
        if not sep or len(h) != 64 or name in seen:
            raise Fail("bad line %r" % l)
        seen[name] = h
    if set(seen) != set(want):
        raise Fail("lists %s, want %s" % (sorted(seen), sorted(want)))
    for name, h in seen.items():
        got = sha(open(want[name], "rb").read())
        if got != h:
            raise Fail("%s: listed %s, file is %s" % (name, h[:16], got[:16]))
    return "; ".join("%s %s" % (n, seen[n][:16]) for n in sorted(seen))


def compare(label, got, want):
    errs = []
    gotd = {}
    for g in got:
        if g["name"] in gotd:
            errs.append("duplicate entry %r" % g["name"])
        gotd[g["name"]] = g
    for w in want:
        g = gotd.pop(w["name"], None)
        if g is None:
            errs.append("missing %r" % w["name"])
            continue
        for k in ("type", "creator", "flags", "dlen", "rlen", "dsha", "rsha"):
            if g[k] != w[k]:
                errs.append("%r %s: got %r, want %r" % (w["name"], k, g[k], w[k]))
    for n in gotd:
        errs.append("unexpected entry %r" % n)
    if errs:
        raise Fail(label + ": " + "; ".join(errs))
    return "%d files: %s" % (len(got), ", ".join("%s %s/%s %04x d%d r%d" % (
        g["name"], g["type"], g["creator"], g["flags"], g["dlen"], g["rlen"]) for g in got))


def verify_all(paths, want, ver):
    """paths: dict dsk/sit/hqx/sums -> file. Returns list of (label, ok, text)."""
    results = []
    jobs = [("hqx via hexbin", "hqx", lambda p: compare("hqx via hexbin", read_hexbin(p, paths["sit"], ver), want)),
            ("hqx via unar", "hqx", lambda p: compare("hqx via unar", read_unar(p), want)),
            ("sit via unar", "sit", lambda p: compare("sit via unar", read_unar(p), want)),
            ("dsk via hfsutils", "dsk", lambda p: compare("dsk via hfsutils", read_dsk(p), want)),
            ("sha256 file", "sums", lambda p: check_sums(p, paths))]
    for label, key, fn in jobs:
        p = paths[key]
        try:
            if not os.path.isfile(p):
                raise Fail("missing " + p)
            results.append((label, True, fn(p)))
        except Fail as e:
            results.append((label, False, str(e)))
        except Exception as e:  # a reader crash is a failure, never a pass
            results.append((label, False, "%s: %s" % (type(e).__name__, e)))
    return results


def report(results):
    ok = True
    for label, good, text in results:
        print("%s %s: %s" % ("PASS" if good else "FAIL", label, text))
        ok = ok and good
    return ok


def build_check(ver):
    inst = open(app_bin("MoofPortInstall"), "rb").read()
    bad = relvers.problems(ver, inst, open(DRV_FLT, "rb").read())
    return bad


# ---- self-test ----

def flip(path, offset):
    with open(path, "r+b") as f:
        f.seek(offset)
        b = f.read(1)
        f.seek(offset)
        f.write(bytes([b[0] ^ 0x01]))


def find_unique(path, needle):
    d = open(path, "rb").read()
    i = d.find(needle)
    if i < 0 or d.find(needle, i + 1) >= 0:
        raise Fail("self-test: cannot place a unique marker in " + path)
    return i


def mksit_path():
    exe = os.path.join(ROOT, "build", "sit", "mksit")
    if not os.path.isfile(exe):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        src = os.path.join(ROOT, "tools", "sit")
        run(["gcc", "-std=c99", "-O2", "-I" + src, "-o", exe] +
            [os.path.join(src, f) for f in ("mksit.c", "classic_sit_writer.c", "stream.c", "sit_crc.c")])
    return exe


def binhex_sit(sit, out, name):
    mb = out + ".bin"
    open(mb, "wb").write(macbinary(name, b"SIT!", b"SIT!", open(sit, "rb").read()))
    body = run(["binhex", mb]).decode("latin-1").split("\n")
    if body and body[0].startswith("("):
        body = body[1:]
    open(out, "w", newline="").write("(This file must be converted with BinHex 4.0)\n" + "\n".join(body))


def wrong_cases(tmp, paths, ver):
    """CRC-valid wrong artifacts: the readers accept them, only the comparison can reject them."""
    mk = mksit_path()
    rm = os.path.join(tmp, "Read Me.bin")
    open(rm, "wb").write(macbinary("Read Me", b"ttro", b"ttxt", readme_text()))
    bins = [app_bin(n) for n, _ in APPS]
    cases = []

    def variant(i, mutate, tag):
        b = bytearray(open(bins[i], "rb").read())
        mutate(b)
        fix_crc(b)
        f = os.path.join(tmp, "%s-%s.bin" % (APPS[i][0], tag))
        open(f, "wb").write(bytes(b))
        return f

    def sit_of(files, tag):
        s = os.path.join(tmp, "w-%s.sit" % tag)
        run([mk, s] + files)
        return s

    def creator(b):
        b[69:73] = b"XXXX"

    def nobundle(b):
        b[73] &= ~0x20

    s = sit_of([bins[0], variant(1, creator, "cre"), bins[2], rm], "creator")
    cases.append(("sit with MoofStat's creator XXXX", dict(paths, sit=s), "sit via unar"))
    s = sit_of([bins[0], bins[1], variant(2, nobundle, "nb"), rm], "bundle")
    cases.append(("sit with MoofTune's bundle bit cleared", dict(paths, sit=s), "sit via unar"))
    s = sit_of(bins, "noreadme")
    cases.append(("sit without the Read Me", dict(paths, sit=s), "sit via unar"))
    txt = open(README_SRC, "rb").read()
    rm2 = os.path.join(tmp, "Read Me2.bin")
    open(rm2, "wb").write(macbinary("Read Me", b"ttro", b"ttxt", readme_text(txt.replace(b"Restart", b"Reboot", 1))))
    s = sit_of(bins + [rm2], "readme")
    cases.append(("sit whose Read Me has one changed word", dict(paths, sit=s), "sit via unar"))
    # a valid .sit.hqx of an archive without MoofTune, paired with a matching .sit (hexbin compare passes)
    s = sit_of([bins[0], bins[1], rm], "short")
    hq = os.path.join(tmp, "w-short.sit.hqx")
    binhex_sit(s, hq, "MoofPort-%s.sit" % ver)
    cases.append(("sit.hqx of an archive without MoofTune", dict(paths, hqx=hq, sit=s), "hqx via hexbin"))
    cases.append(("sit.hqx of an archive without MoofTune", dict(paths, hqx=hq), "hqx via unar"))
    # a valid .sit.hqx of the right archive under another name, and one that does not match the .sit
    hq = os.path.join(tmp, "w-name.sit.hqx")
    binhex_sit(paths["sit"], hq, "MoofPort-0.9.sit")
    cases.append(("sit.hqx whose inner file is named MoofPort-0.9.sit", dict(paths, hqx=hq), "hqx via hexbin"))
    s = sit_of(bins + [rm], "fresh")       # the same content, new dates: not byte for byte the .sit
    hq = os.path.join(tmp, "w-other.sit.hqx")
    binhex_sit(s, hq, "MoofPort-%s.sit" % ver)
    if open(s, "rb").read() != open(paths["sit"], "rb").read():
        cases.append(("sit.hqx that is not the .sit beside it", dict(paths, hqx=hq), "hqx via hexbin"))
    trunc = os.path.join(tmp, "trunc.dsk")
    shutil.copy(paths["dsk"], trunc)
    os.truncate(trunc, DSK_SIZE - 1)
    cases.append(("dsk truncated by one byte", dict(paths, dsk=trunc), "dsk via hfsutils"))
    inv = os.path.join(tmp, "inv.dsk")
    shutil.copy(paths["dsk"], inv)
    hfs(["hmount", inv])
    try:
        hfs(["hcopy", "-m", rm, ":Desktop"])
        hfs(["hattrib", "+i", ":Desktop"])
    finally:
        humount()
    cases.append(("dsk with an extra invisible file", dict(paths, dsk=inv), "dsk via hfsutils"))
    sums = os.path.join(tmp, "bad.sha256")
    lines = open(paths["sums"]).read().split("\n")
    lines[0] = ("0" if lines[0][0] != "0" else "1") + lines[0][1:]
    open(sums, "w").write("\n".join(lines))
    cases.append(("sha256 file with one wrong digit", dict(paths, sums=sums), "sha256 file"))
    sums = os.path.join(tmp, "short.sha256")
    open(sums, "w").write("\n".join(open(paths["sums"]).read().split("\n")[1:]))
    cases.append(("sha256 file missing a line", dict(paths, sums=sums), "sha256 file"))
    return cases


def self_test(paths, want, ver):
    if not report(verify_all(paths, want, ver)):
        print("SELF-TEST ABORT: the pristine artifacts do not pass")
        return False
    inst = open(app_bin("MoofPortInstall"), "rb").read()
    # a 48-byte piece of the driver code inside the installer's resource fork that occurs once in
    # both the .sit and the .dsk (the apps share library code, so not every piece is unique)
    drv = open(DRV_FLT, "rb").read()
    sitb, dskb = open(paths["sit"], "rb").read(), open(paths["dsk"], "rb").read()
    marker = None
    for o in range(len(drv) // 2, len(drv) - 48, 16):
        m = drv[o:o + 48]
        if all(b.count(m) == 1 for b in (sitb, dskb, inst)):
            marker = m
            break
    if marker is None:
        print("SELF-TEST ABORT: no unique piece of the driver in the artifacts")
        return False
    tmp = tempfile.mkdtemp(prefix="relv-self-")
    good = True
    try:
        cases = []
        c = dict(paths, sit=os.path.join(tmp, "bad.sit"))
        shutil.copy(paths["sit"], c["sit"])
        flip(c["sit"], find_unique(c["sit"], marker) + 7)
        cases.append(("sit resource-fork byte", c, "sit via unar"))
        c = dict(paths, dsk=os.path.join(tmp, "bad.dsk"))
        shutil.copy(paths["dsk"], c["dsk"])
        flip(c["dsk"], find_unique(c["dsk"], marker) + 7)
        cases.append(("dsk resource-fork byte", c, "dsk via hfsutils"))
        c = dict(paths, dsk=os.path.join(tmp, "badtype.dsk"))
        shutil.copy(paths["dsk"], c["dsk"])
        hfs(["hmount", c["dsk"]])
        try:
            hfs(["hattrib", "-t", "XXXX", ":Read Me"])
        finally:
            humount()
        cases.append(("dsk Read Me type code", c, "dsk via hfsutils"))
        c = dict(paths, hqx=os.path.join(tmp, "bad.sit.hqx"))
        d = bytearray(open(paths["hqx"], "rb").read())
        i = len(d) // 2
        while d[i] in b"\r\n:":
            i += 1
        d[i] = ord("!") if d[i] != ord("!") else ord('"')
        open(c["hqx"], "wb").write(d)
        cases.append(("hqx body character", c, "hqx via hexbin"))
        cases.append(("hqx body character", c, "hqx via unar"))
        cases += wrong_cases(tmp, paths, ver)
        for name, cpaths, label in cases:
            res = [r for r in verify_all(cpaths, want, ver) if r[0] == label]
            if res[0][1]:
                print("SELF-TEST FAIL: corrupted %s still passes (%s)" % (name, label))
                good = False
            else:
                print("SELF-TEST ok: %s fails (%s): %s" % (name, label, res[0][2][:110]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # relvers: a wrong version, another driver, no log line, a diagnostic build
    drv = open(DRV_FLT, "rb").read()
    for name, v, blob, d in (
            ("wrong release version", ver + "x", inst, drv),
            ("driver one byte different", ver, inst, drv[:-1] + bytes([drv[-1] ^ 1])),
            ("installer without the log line (one byte changed)", ver, inst.replace(
                ("MoofPortInstall %s\r" % ver).encode(), ("MoofPortInstall %s_" % ver).encode(), 1), drv),
            ("installer containing the word 'diagnostic'", ver, inst + b"diagnostic", drv)):
        bad = relvers.problems(v, blob, d)
        if bad:
            print("SELF-TEST ok: relvers rejects the %s: %s" % (name, bad[0][:100]))
        else:
            print("SELF-TEST FAIL: relvers accepts the %s" % name)
            good = False
    return good


def main():
    args = sys.argv[1:]
    st = False
    dist = os.path.join(ROOT, "dist")
    while args and args[0].startswith("--"):
        a = args.pop(0)
        if a == "--self-test":
            st = True
        elif a == "--dist" and args:
            dist = args.pop(0)
        else:
            sys.exit(__doc__)
    if len(args) != 1:
        sys.exit(__doc__)
    ver = args[0]
    base = os.path.join(dist, "MoofPort-" + ver)
    paths = dict(dsk=base + ".dsk", sit=base + ".sit", hqx=base + ".sit.hqx", sums=base + ".sha256")
    for tool in ("hexbin", "binhex", "unar", "lsar", "hmount", "hls", "hcopy", "hattrib", "humount", "gcc"):
        if shutil.which(tool) is None:
            sys.exit("relverify.py: %s not installed (docs/dev/RELEASE.md)" % tool)
    try:
        try:
            want = expected()
        except Fail as e:
            sys.exit("relverify.py: " + str(e))
        bad = build_check(ver)
        print("%s build: %s" % ("FAIL" if bad else "PASS", "; ".join(bad) if bad else
                                "MoofPortInstall %s: vers, log line, no diagnostic build, 'MPDV' 128 = MoofPortDrv.flt" % ver))
        if st:
            ok = self_test(paths, want, ver)
        else:
            ok = report(verify_all(paths, want, ver))
        ok = ok and not bad
        print("RESULT " + ("PASS" if ok else "FAIL"))
        sys.exit(0 if ok else 1)
    finally:
        shutil.rmtree(HFS_HOME, ignore_errors=True)


if __name__ == "__main__":
    main()

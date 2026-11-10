#!/usr/bin/env python3
"""hw-bundle.py - pack a staged hardware session into one StuffIt archive, BinHexed, and verify it.

    scripts/hw-bundle.py <session> [--lan IP --router IP --mac IP] [--set NAME=TEXT]...

Reads what scripts/stage-hw.sh put in dist/hw/<session>/ (the tools' MacBinary files and
CHECKLIST.md) and writes dist/hw/<session>/MoofPort-<session>.sit.hqx, plus the bare .sit beside
it. The archive is FLAT, the layout of MoofTCP's releases: StuffIt Expander or StuffIt Lite expands
it into the folder you pick (make it "MoofPort Test" on the hard disk). Entries, in this order:

    Read Me              the checklist (TEXT/ttxt)
    the staged tools     every .bin stage-hw.sh staged (MoofPortInstall and SpyENET first)
    the settings files   of the staged tools only, and any --set NAME=TEXT (one line, e.g.
                         --set "AfpCopy.cfg=volume=Shared")

No folders: an archive with folder entries hung StuffIt Lite 3.6 on a real Classic
(2026-11-06). The writer's folder markers are 0x30/0x31 where StuffIt 1.5.1 uses 32/33, and its
header counts every record where the folder layout may want the top-level count; which of those
(if either) caused the hang is unproven, so folders stay out until a real StuffIt reads them.

A settings file whose addresses were given is filled in under its real name: TcpBench.cfg needs
--lan, EnetBench.cfg and ScsiProbe.cfg need --router and --mac. Without them it is a template
named "<name> template" plus "Settings Read Me", so the tool never reads a placeholder (it would
silently use its emulator default).

Method (MoofTCP's scripts/release.sh and relverify.py): tools/sit/mksit, SitStill's stored Classic
SIT writer, builds the archive; macutils binhex encodes it, with its first comment line replaced
by the standard one so every decoder recognises it. Verification uses readers that did not write
it: hexbin must give back the identical .sit, and lsar/unar must list and extract every file with
the expected name and path, type/creator, Finder flags (all bits but 0x0100, which the Finder sets
by itself), fork lengths and sha256 of both forks. A copy with one resource-fork byte changed must
then FAIL that check. Exit 0 only if all of that holds.
"""
import argparse
import hashlib
import ipaddress
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRST = ["MoofPortInstall", "SpyENET"]   # these lead the archive, then the rest by name
FLAG_MASK = 0xFEFF
MAC_EPOCH = 2082844800


class Fail(Exception):
    pass


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode != 0:
        raise Fail("%s failed: %s" % (cmd[0], r.stderr.decode(errors="replace").strip()))
    return r.stdout


def crc16x(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def rec(path, typ, creator, flags, data, rsrc):
    return dict(path=path, type=typ, creator=creator, flags=flags & FLAG_MASK, dlen=len(data),
                rlen=len(rsrc), dsha=hashlib.sha256(data).hexdigest(),
                rsha=hashlib.sha256(rsrc).hexdigest())


def parse_macbinary(blob):
    n = blob[1]
    dl, rl = struct.unpack(">II", blob[83:91])
    ro = 128 + (dl + 127) // 128 * 128
    return dict(name=blob[2:2 + n].decode("mac_roman"), type=blob[65:69].decode("mac_roman"),
                creator=blob[69:73].decode("mac_roman"), flags=(blob[73] << 8) | blob[101],
                data=blob[128:128 + dl], rsrc=blob[ro:ro + rl])


def macbinary(name, typ, creator, data, rsrc=b""):
    now = int(time.time()) + MAC_EPOCH
    h = bytearray(128)
    nb = name.encode("mac_roman")
    h[1] = len(nb)
    h[2:2 + len(nb)] = nb
    h[65:69] = typ
    h[69:73] = creator
    h[83:91] = struct.pack(">II", len(data), len(rsrc))
    h[91:99] = struct.pack(">II", now, now)
    h[122] = 129
    h[123] = 129
    h[124:126] = struct.pack(">H", crc16x(bytes(h[:124])))
    pad = lambda b: b + b"\0" * (-len(b) % 128)
    return bytes(h) + pad(data) + pad(rsrc)


def text(s):
    b = s.replace("\r\n", "\n").encode("ascii")
    if any(x > 126 or (x < 32 and x != 10) for x in b):
        raise Fail("text for the Mac must be plain ASCII")
    return b.replace(b"\n", b"\r")


def appledouble_rsrc(path):
    b = open(path, "rb").read()
    if b[:4] != b"\x00\x05\x16\x07":
        raise Fail("%s is not AppleDouble" % path)
    for i in range(struct.unpack(">H", b[24:26])[0]):
        eid, off, ln = struct.unpack(">III", b[26 + 12 * i:38 + 12 * i])
        if eid == 2:
            return b[off:off + ln]
    return b""


def read_unar(archive):
    """Every file in the archive, as lsar lists it and unar extracts it."""
    meta = json.loads(run(["lsar", "-j", archive]), strict=False)["lsarContents"]
    out = {}
    for e in meta:
        if e.get("XADIsDirectory"):
            continue
        o = out.setdefault(e["XADFileName"], dict(type=None, creator=None, flags=0))
        if "XADFileType" in e:
            o["type"] = struct.pack(">I", e["XADFileType"]).decode("mac_roman")
            o["creator"] = struct.pack(">I", e["XADFileCreator"]).decode("mac_roman")
            o["flags"] |= e.get("XADFinderFlags", 0)
    tmp = tempfile.mkdtemp(prefix="hwb-unar-")
    try:
        run(["unar", "-q", "-D", "-k", "visible", "-o", tmp, archive])
        res = []
        for nm, o in out.items():
            p = os.path.join(tmp, nm)
            data = open(p, "rb").read() if os.path.isfile(p) else b""
            rsrc = appledouble_rsrc(p + ".rsrc") if os.path.isfile(p + ".rsrc") else b""
            res.append(rec(nm, o["type"], o["creator"], o["flags"], data, rsrc))
        return res
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def compare(got, want):
    errs = []
    gotd = {g["path"]: g for g in got}
    for w in want:
        g = gotd.pop(w["path"], None)
        if g is None:
            errs.append("missing %r" % w["path"])
            continue
        for k in ("type", "creator", "flags", "dlen", "rlen", "dsha", "rsha"):
            if g[k] != w[k]:
                errs.append("%r %s: got %r, want %r" % (w["path"], k, g[k], w[k]))
    errs += ["unexpected entry %r" % n for n in gotd]
    if errs:
        raise Fail("; ".join(errs))


def ip(s):
    return str(ipaddress.IPv4Address(s))


def build_mksit():
    out = os.path.join(ROOT, "build", "sit")
    os.makedirs(out, exist_ok=True)
    t = os.path.join(ROOT, "tools", "sit")
    exe = os.path.join(out, "mksit")
    run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2", "-I" + t, "-o", exe] +
        [os.path.join(t, f) for f in ("mksit.c", "classic_sit_writer.c", "stream.c", "sit_crc.c")])
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("session")
    ap.add_argument("--lan", type=ip, help="machine running scripts/testnet.py")
    ap.add_argument("--router", type=ip, help="router, from the MacTCP control panel")
    ap.add_argument("--mac", type=ip, help="this Mac's address, from the MacTCP control panel")
    ap.add_argument("--set", action="append", default=[], metavar="NAME=TEXT",
                    help="an extra settings file NAME holding the line TEXT")
    ap.add_argument("--readme", metavar="FILE",
                    help="use FILE as the Read Me (a release) instead of the session checklist")
    ap.add_argument("--stem", help="archive name without .sit.hqx (default MoofPort-<session>)")
    a = ap.parse_args()
    if bool(a.router) != bool(a.mac):
        sys.exit("hw-bundle.py: --router and --mac go together (EnetBench.cfg and ScsiProbe.cfg need both)")
    for t in ("gcc", "binhex", "hexbin", "unar", "lsar"):
        if not shutil.which(t):
            sys.exit("hw-bundle.py: %s not installed" % t)

    sess = a.session
    src = os.path.join(ROOT, "dist", "hw", sess)
    if not os.path.isdir(src):
        sys.exit("hw-bundle.py: no staged session %s (run scripts/stage-hw.sh first)" % src)
    stem = a.stem or "MoofPort-" + sess

    tools = sorted(f[:-4] for f in os.listdir(src) if f.endswith(".bin"))
    tools = [t for t in FIRST if t in tools] + [t for t in tools if t not in FIRST]
    if not tools:
        sys.exit("hw-bundle.py: no tools (.bin) in %s" % src)
    cfg = [("TcpBench.cfg", a.lan, "LAN_MACHINE_IP"),
           ("EnetBench.cfg", a.router and "%s %s" % (a.router, a.mac), "ROUTER_IP MAC_IP"),
           ("ScsiProbe.cfg", a.mac and "142 %s %s" % (a.mac, a.router), "142 MAC_IP ROUTER_IP")]
    cfg = [c for c in cfg if c[0][:-4] in tools]
    for kv in a.set:
        if "=" not in kv:
            sys.exit("hw-bundle.py: --set wants NAME=TEXT, got %r" % kv)
        n, v = kv.split("=", 1)
        cfg = [c for c in cfg if c[0] != n]      # --set replaces a tool's template of that name
        cfg.append((n, v, None))
    done = [(n, v) for n, v, _ in cfg if v]
    todo = [(n, t) for n, v, t in cfg if not v]
    settings_readme = (
        "The files named \"... template\" are settings files to fill in. Before the step that\n"
        "needs one, open it in TeachText, replace the capital-letter words with the dotted\n"
        "addresses from Install step 5, and save it as text under the name without\n"
        "\" template\" (for example TcpBench.cfg), next to its application.\n\n"
        "LAN_MACHINE_IP  the machine running scripts/testnet.py\n"
        "ROUTER_IP       the router shown in the MacTCP control panel\n"
        "MAC_IP          this Mac's address shown in the MacTCP control panel\n\n"
        "TcpBench.cfg    LAN_MACHINE_IP\n"
        "EnetBench.cfg   ROUTER_IP MAC_IP        (router first)\n"
        "ScsiProbe.cfg   142 MAC_IP ROUTER_IP    (Mac first, the opposite order)\n\n"
        "ScsiProbe.cfg: the first number changes between runs (step 14): 142, then 230, then\n"
        "150. A file with the capital words left in gives no error: the tool silently uses\n"
        "emulator addresses and gets no replies from your network.\n")
    # (the table above lists every template; only the staged tools' files are in the archive)
    if a.readme:
        checklist = ""
        readme = open(a.readme).read()
    else:
        checklist = open(os.path.join(src, "CHECKLIST.md")).read()
        readme = ("This folder holds every file for hardware session %s. The steps below are the\n"
                  "checklist; the copy on the host (CHECKLIST.md) is the same text.\n" % sess)
    if done:
        readme += ("Settings files already filled in: %s.\n" % ", ".join(
            "%s (%s)" % (n, v) for n, v in done))
    if todo:
        readme += ("Still to fill in: %s; see Settings Read Me.\n" % ", ".join(n for n, _ in todo))
    if checklist:
        readme += "\n" + checklist

    tmp = tempfile.mkdtemp(prefix="hwb-")
    try:
        want, args = [], []

        def add(path, blob, binname):
            p = os.path.join(tmp, binname)
            open(p, "wb").write(blob)
            m = parse_macbinary(blob)
            want.append(rec(path, m["type"], m["creator"], m["flags"], m["data"], m["rsrc"]))
            args.append(p)

        def add_text(path, name, body):
            add(path, macbinary(name, b"TEXT", b"ttxt", text(body)), "t%d.bin" % len(want))

        def add_tool(path, name):
            add(path, open(os.path.join(src, name + ".bin"), "rb").read(), name + ".bin")

        add_text("Read Me", "Read Me", readme)
        for n in tools:
            add_tool(n, n)
        for n, v in done:
            add_text(n, n, v + "\n")
        for n, t in todo:
            add_text(n + " template", n + " template", t + "\n")
        if todo:
            add_text("Settings Read Me", "Settings Read Me", settings_readme)

        mksit = build_mksit()
        sit = os.path.join(src, stem + ".sit")
        run([mksit, sit] + args)
        sit_bytes = open(sit, "rb").read()

        mb = os.path.join(tmp, stem + ".sit.bin")
        open(mb, "wb").write(macbinary(stem + ".sit", b"SIT!", b"SIT!", sit_bytes))
        body = run(["binhex", mb]).decode("ascii").splitlines(True)
        if body and body[0].startswith("("):
            body = body[1:]
        hqx = os.path.join(src, stem + ".sit.hqx")
        with open(hqx, "w", newline="\n") as f:
            f.write("(This file must be converted with BinHex 4.0)\n" + "".join(body))

        # Verify with readers that did not write it.
        d = tempfile.mkdtemp(dir=tmp)
        run(["hexbin", hqx], cwd=d)
        got = os.listdir(d)
        if len(got) != 1:
            raise Fail("hexbin produced %r" % got)
        m = parse_macbinary(open(os.path.join(d, got[0]), "rb").read())
        if (m["type"], m["creator"], m["rsrc"]) != ("SIT!", "SIT!", b"") or m["data"] != sit_bytes:
            raise Fail("hexbin did not give back the identical .sit")
        compare(read_unar(sit), want)
        compare(read_unar(hqx), want)
        print("verify: hexbin gives back the identical .sit; lsar/unar read %d files from the .sit "
              "and the .sit.hqx, all equal" % len(want))

        # Self-test: a changed resource-fork byte must be caught.
        bad = os.path.join(tmp, "bad.sit")
        b = bytearray(sit_bytes)
        probe = next(t for t in tools if len(parse_macbinary(
            open(os.path.join(src, t + ".bin"), "rb").read())["rsrc"]) >= 288)
        spy = parse_macbinary(open(os.path.join(src, probe + ".bin"), "rb").read())
        i = b.find(spy["rsrc"][256:288])
        if i < 0:
            raise Fail("self-test could not find %s's resource fork in the archive" % probe)
        b[i] ^= 0xFF
        open(bad, "wb").write(bytes(b))
        try:
            compare(read_unar(bad), want)
        except Fail:
            print("self-test: a copy with one resource-fork byte changed fails, as it must")
        else:
            raise Fail("self-test: a corrupted archive passed")

        for w in want:
            print("  %-36s %s/%s flags %04x data %6d rsrc %6d" %
                  (w["path"], w["type"], w["creator"], w["flags"], w["dlen"], w["rlen"]))
        print("wrote %s (%d bytes; .sit %d bytes; filled in: %s; templates: %s)" %
              (hqx, os.path.getsize(hqx), len(sit_bytes), ", ".join(n for n, _ in done) or "none",
               ", ".join(n for n, _ in todo) or "none"))
    except Fail as e:
        sys.exit("hw-bundle.py: FAIL: %s" % e)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()

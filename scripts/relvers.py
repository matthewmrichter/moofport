#!/usr/bin/env python3
"""Release version and identity check (scripts/release.sh, scripts/relverify.py).

    relvers.py <version> <MoofPortInstall.bin> <MoofPortDrv.flt>

Port of MoofTCP's scripts/relvers.py for MoofPort's installer. Exit 1 unless all of these hold:
  - the installer's 'vers' 1 short string is <version>, and its long string is
    "MoofPort <version> installer" (no "diagnostic");
  - the installer's code carries the log line "MoofPortInstall <version>" followed by CR (the
    first line of Installer-<name>.out), and the word "diagnostic" appears nowhere in the file
    (MoofPortDiag, the -DMP_DIAG build of the same source, logs "MoofPortInstall <v> diagnostic");
  - 'MPDV' 128 (the driver the installer copies into the System file as DRVR 47) is the
    50-byte driver header plus build-mac/MoofPortDrv.flt byte for byte (the release driver, not
    MoofPortDiagDrv.flt), and the flat code holds no "mp_diag" text.
The resource-map reader here is also used by scripts/relverify.py.
"""
import struct
import sys

DRVR_HDR = 50          # flags, offsets, name, six stubs (mac/driver/mp_drvr.r), then the flat code


class Bad(Exception):
    pass


def macbinary(blob, what="file"):
    """(name, type, creator, flags, data, rsrc) of a MacBinary blob."""
    if len(blob) < 128 or blob[0] != 0 or blob[74] != 0 or not 1 <= blob[1] <= 63:
        raise Bad("%s: not MacBinary" % what)
    dl, rl = struct.unpack(">II", blob[83:91])
    ro = 128 + (dl + 127) // 128 * 128
    if 128 + dl > len(blob) or ro + rl > len(blob):
        raise Bad("%s: truncated MacBinary" % what)
    return (blob[2:2 + blob[1]].decode("mac_roman"), blob[65:69].decode("mac_roman"),
            blob[69:73].decode("mac_roman"), (blob[73] << 8) | blob[101],
            blob[128:128 + dl], blob[ro:ro + rl])


def resources(rsrc):
    """{(type, id): data} of a resource fork (Inside Macintosh: More Toolbox, Resource Manager)."""
    if len(rsrc) < 16:
        raise Bad("resource fork too short")
    doff, moff, dlen, mlen = struct.unpack(">IIII", rsrc[:16])
    m = rsrc[moff:moff + mlen]
    if len(m) < 30 or doff + dlen > len(rsrc):
        raise Bad("resource map out of range")
    tl_off = struct.unpack(">H", m[24:26])[0]
    ntypes = struct.unpack(">H", m[tl_off:tl_off + 2])[0] + 1
    out = {}
    for i in range(ntypes):
        e = tl_off + 2 + 8 * i
        t = m[e:e + 4].decode("mac_roman")
        n, ref = struct.unpack(">HH", m[e + 4:e + 8])
        for j in range(n + 1):
            r = tl_off + ref + 12 * j
            rid = struct.unpack(">h", m[r:r + 2])[0]
            off = struct.unpack(">I", b"\0" + m[r + 5:r + 8])[0]
            p = doff + off
            ln = struct.unpack(">I", rsrc[p:p + 4])[0]
            out[(t, rid)] = rsrc[p + 4:p + 4 + ln]
    return out


def vers1(res):
    d = res.get(("vers", 1))
    if d is None:
        raise Bad("no 'vers' 1")
    n = d[6]
    m = d[7 + n]
    return d[7:7 + n].decode("mac_roman"), d[8 + n:8 + n + m].decode("mac_roman")


def problems(ver, inst_blob, drv_flt):
    """List of problems (empty = good)."""
    bad = []
    try:
        name, typ, cre, flags, _data, rsrc = macbinary(inst_blob, "installer")
        res = resources(rsrc)
        short, long_ = vers1(res)
    except Bad as e:
        return ["installer: %s" % e]
    if short != ver:
        bad.append("installer 'vers' 1 short string is %r, release is %r" % (short, ver))
    if long_ != "MoofPort %s installer" % ver:
        bad.append("installer 'vers' 1 long string is %r, want %r" % (long_, "MoofPort %s installer" % ver))
    line = ("MoofPortInstall %s\r" % ver).encode("mac_roman")
    if rsrc.count(line) != 1:
        bad.append("installer code has the log line %r %d times, want once (mac/installer/installer.c MP_INST_VER)"
                   % (line, rsrc.count(line)))
    if b"diagnostic" in inst_blob.lower():
        bad.append("the installer contains the word 'diagnostic': an MP_DIAG build (MoofPortDiag)?")
    mpdv = res.get(("MPDV", 128))
    if mpdv is None:
        bad.append("no 'MPDV' 128 (the driver) in the installer")
    elif mpdv[DRVR_HDR:] != drv_flt:
        bad.append("'MPDV' 128 is not the driver header + MoofPortDrv.flt (%d vs %d code bytes)"
                   % (len(mpdv) - DRVR_HDR, len(drv_flt)))
    elif mpdv[18:25] != b"\x06.ENET0":
        bad.append("'MPDV' 128 does not name .ENET0")
    if b"mp_diag" in drv_flt:
        bad.append("MoofPortDrv.flt contains 'mp_diag'")
    return bad


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    ver, inst, drv = sys.argv[1:]
    bad = problems(ver, open(inst, "rb").read(), open(drv, "rb").read())
    for b in bad:
        print("relvers.py: " + b, file=sys.stderr)
    if not bad:
        print("relvers.py: installer %s: vers, log line, release driver OK" % ver)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

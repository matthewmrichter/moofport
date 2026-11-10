#!/usr/bin/env bash
# Build the MoofPort release files (port of MoofTCP's scripts/release.sh; docs/dev/RELEASE.md).
#   release.sh [--force] [--no-build] <version>        e.g. release.sh 1.0
# Output (dist/ is gitignored; existing outputs are refused without --force):
#   dist/MoofPort-<v>.dsk       800K HFS image, volume "MoofPort": MoofPortInstall, MoofStat, MoofTune, Read Me
#   dist/MoofPort-<v>.sit       StuffIt 1.5.1-compatible archive of the same four files, both forks,
#                               type/creator and Finder flags (tools/sit/mksit, stored, flat: no folder)
#   dist/MoofPort-<v>.sit.hqx   BinHex 4.0 of that .sit (file "MoofPort-<v>.sit", SIT!/SIT!; macutils binhex)
#   dist/MoofPort-<v>.sha256    sha256sum lines of the three files above (sha256sum -c)
# Steps: Retro68 build of MoofPortInstall, MoofStat and MoofTune; release-build checks (no MP_DIAG in the
# configured flags, no libgcc helper or mp_diag symbol in build-mac/MoofPortDrv.flt.gdb, scripts/relvers.py:
# vers and log line "MoofPortInstall <v>", no diagnostic build, 'MPDV' 128 = MoofPortDrv.flt; the MacBinary
# identity of each app); "Read Me" from docs/release/ReadMe.txt (CR, ttro/ttxt); the four files; then
# scripts/release-verify.sh (independent readers). --no-build reuses build-mac/ (the checks still run).
# Needs docker (Retro68 image), hfsutils, macutils, unar, gcc, python3, sha256sum.
# hfsutils runs with HOME set to the staging directory (its ~/.hcwd must not clash with other jobs).
# scripts/hw-bundle.py (hardware kits) is separate and unchanged; this script no longer calls it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "release.sh: $*" >&2; exit 1; }

force=""; nobuild=""
while [ $# -gt 0 ]; do
  case "$1" in
    --force) force=1; shift;;
    --no-build) nobuild=1; shift;;
    -*) die "unknown option $1";;
    *) break;;
  esac
done
[ $# -eq 1 ] || die "usage: release.sh [--force] [--no-build] <version>"
ver="$1"
case "$ver" in ""|.*|*[!A-Za-z0-9._-]*) die "version must match [A-Za-z0-9._-] and not start with '.'";; esac
for t in docker hformat hmount hcopy hls humount binhex hexbin unar lsar gcc python3 sha256sum; do
  command -v "$t" >/dev/null || die "$t not installed (see docs/dev/RELEASE.md)"
done

APPS=(MoofPortInstall MoofStat MoofTune)
dist="$ROOT/dist"
base="$dist/MoofPort-$ver"; volname="MoofPort"
outs=("$base.dsk" "$base.sit" "$base.sit.hqx" "$base.sha256")
if [ -z "$force" ]; then
  for f in "${outs[@]}"; do [ ! -e "$f" ] || die "$f exists; use --force to replace it"; done
fi
mkdir -p "$dist"
stage="$(mktemp -d "$dist/.stage-XXXXXX")"
trap 'rm -rf "$stage"' EXIT
hfs() { HOME="$stage" "$@"; }

cd "$ROOT"
# 1. Release build (build-mac.sh configures with -m68000 only; the MP_DIAG targets are separate).
if [ -z "$nobuild" ]; then
  for t in "${APPS[@]}"; do scripts/build-mac.sh "$t" >/dev/null || die "build of $t failed"; done
fi
for t in "${APPS[@]}"; do [ -f "build-mac/$t.bin" ] || die "missing build-mac/$t.bin (build without --no-build)"; done
[ -f build-mac/MoofPortDrv.flt.gdb ] || die "missing build-mac/MoofPortDrv.flt.gdb"

# 2. Release-build checks.
flags="$(grep -E '^CMAKE_C_FLAGS:' build-mac/CMakeCache.txt || true)"
case "$flags" in *MP_DIAG*) die "build-mac/ was configured with MP_DIAG ($flags)";; esac
# relative path: the container sees the repo at /src
syms="$(scripts/r68 m68k-apple-macos-nm build-mac/MoofPortDrv.flt.gdb)" || die "nm failed on MoofPortDrv.flt.gdb"
[ "$(wc -l <<<"$syms")" -gt 20 ] || die "nm output of MoofPortDrv.flt.gdb looks empty"
# The driver is position-independent and not relocated: a libgcc helper (__divsi3, __mulsi3, ...) is
# linked at an absolute address and crashes it (a helper crashed an early diagnostic build).
helpers="$(grep -E ' T __[a-z]+[sd]i3$' <<<"$syms" || true)"
[ -z "$helpers" ] || die "libgcc helpers linked into the driver: $helpers"
if grep -qi 'mp_diag' <<<"$syms"; then die "MoofPortDrv.flt.gdb has mp_diag symbols: a diagnostic driver"; fi
python3 -I scripts/relvers.py "$ver" build-mac/MoofPortInstall.bin build-mac/MoofPortDrv.flt \
  || die "the installer is not a release build of $ver (mac/installer: vers, MP_INST_VER)"
python3 -I - build-mac/MoofPortInstall.bin MPIN build-mac/MoofStat.bin MPST build-mac/MoofTune.bin MPTU <<'PY' \
  || die "app MacBinary identity check failed"
import os, sys
a = sys.argv[1:]
for path, cre in zip(a[0::2], a[1::2]):
    d = open(path, "rb").read()
    name, typ, c, flags = d[2:2 + d[1]], d[65:69], d[69:73], (d[73] << 8) | d[101]
    want = os.path.basename(path)[:-4].encode()
    if (name, typ, c) != (want, b"APPL", cre.encode()) or not flags & 0x2000:
        sys.exit("%s: %r %r %r flags %04x" % (path, name, typ, c, flags))
PY

# 3. Stage: the apps + "Read Me" as MacBinary (plain ASCII, at most 64 columns, CR, ttro/ttxt).
for t in "${APPS[@]}"; do cp "build-mac/$t.bin" "$stage/$t.bin"; done
python3 -I - docs/release/ReadMe.txt "$stage/Read Me.bin" <<'PY'
import struct, sys, time
src, dst = sys.argv[1:3]
txt = open(src, "rb").read()
if any(b > 126 or (b < 32 and b not in (10, 13)) for b in txt):
    sys.exit("ReadMe.txt must be plain ASCII")
long_ = [i + 1 for i, l in enumerate(txt.split(b"\n")) if len(l) > 64]
if long_:
    sys.exit("ReadMe.txt lines over 64 columns: %s" % long_[:10])
if not txt.endswith(b"\n"):
    txt += b"\n"
data = txt.replace(b"\r\n", b"\n").replace(b"\n", b"\r")
def crc16(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c
now = int(time.time()) + 2082844800
h = bytearray(128)
nm = b"Read Me"
h[1] = len(nm); h[2:2 + len(nm)] = nm
h[65:69] = b"ttro"; h[69:73] = b"ttxt"
h[83:87] = struct.pack(">I", len(data))
h[91:95] = struct.pack(">I", now); h[95:99] = struct.pack(">I", now)
h[122] = 129; h[123] = 129
h[124:126] = struct.pack(">H", crc16(h[:124]))
open(dst, "wb").write(bytes(h) + data + b"\0" * (-len(data) % 128))
PY
files=("$stage/MoofPortInstall.bin" "$stage/MoofStat.bin" "$stage/MoofTune.bin" "$stage/Read Me.bin")

# 4. .dsk: 800K HFS volume "MoofPort" (hcopy -m keeps forks, type/creator and Finder flags).
dsk="$stage/out.dsk"
dd if=/dev/zero of="$dsk" bs=1024 count=800 status=none
hfs hformat -l "$volname" "$dsk" >/dev/null
hfs hmount "$dsk" >/dev/null
trap 'HOME="$stage" humount >/dev/null 2>&1 || true; rm -rf "$stage"' EXIT
for f in "${files[@]}"; do n="$(basename "$f" .bin)"; hfs hcopy -m "$f" ":$n"; done
hfs hls -l :
hfs humount >/dev/null
trap 'rm -rf "$stage"' EXIT

# 5. .sit (host-built writer, tools/sit; flat, no folder entries: see scripts/hw-bundle.py)
sitdir="$ROOT/build/sit"
mkdir -p "$sitdir"
gcc -std=c99 -Wall -Wextra -Werror -O2 -I"$ROOT/tools/sit" -o "$sitdir/mksit" \
  "$ROOT/tools/sit/mksit.c" "$ROOT/tools/sit/classic_sit_writer.c" "$ROOT/tools/sit/stream.c" "$ROOT/tools/sit/sit_crc.c"
"$sitdir/mksit" "$stage/out.sit" "${files[@]}"

# 6. .sit.hqx: the .sit as a MacBinary file "MoofPort-<v>.sit" (SIT!/SIT!, data fork only), BinHexed.
# macutils binhex starts with its own comment line, which unar does not recognise; use the standard one.
python3 -I - "$stage/out.sit" "$stage/sit.bin" "MoofPort-$ver.sit" <<'PY'
import struct, sys, time
src, dst, name = sys.argv[1:4]
data = open(src, "rb").read()
def crc16(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c
now = int(time.time()) + 2082844800
h = bytearray(128)
nm = name.encode()
h[1] = len(nm); h[2:2 + len(nm)] = nm
h[65:69] = b"SIT!"; h[69:73] = b"SIT!"
h[83:87] = struct.pack(">I", len(data))
h[91:95] = struct.pack(">I", now); h[95:99] = struct.pack(">I", now)
h[122] = 129; h[123] = 129
h[124:126] = struct.pack(">H", crc16(h[:124]))
open(dst, "wb").write(bytes(h) + data + b"\0" * (-len(data) % 128))
PY
binhex "$stage/sit.bin" > "$stage/body.hqx"
[ -s "$stage/body.hqx" ] || die "binhex produced an empty file"
{ echo "(This file must be converted with BinHex 4.0)"; sed '1{/^(/d;}' "$stage/body.hqx"; } > "$stage/out.sit.hqx"

# 7. Checksums, then publish (replace only after everything was made), then verify.
cp "$stage/out.dsk" "$stage/MoofPort-$ver.dsk"
cp "$stage/out.sit" "$stage/MoofPort-$ver.sit"
cp "$stage/out.sit.hqx" "$stage/MoofPort-$ver.sit.hqx"
(cd "$stage" && sha256sum "MoofPort-$ver.dsk" "MoofPort-$ver.sit" "MoofPort-$ver.sit.hqx") > "$stage/out.sha256"
mv -f "$stage/out.dsk" "$base.dsk"
mv -f "$stage/out.sit" "$base.sit"
mv -f "$stage/out.sit.hqx" "$base.sit.hqx"
mv -f "$stage/out.sha256" "$base.sha256"
ls -l "${outs[@]}"
cat "$base.sha256"
"$ROOT/scripts/release-verify.sh" "$ver"

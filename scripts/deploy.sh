#!/usr/bin/env bash
# deploy.sh - copy MacBinary builds onto work.hda with hfsutils.
#   deploy.sh <file.bin>... --to ":Folder:"
#
# Port of MoofTCP's scripts/deploy.sh (EMULATOR.md "Usage"). It refuses while Snow
# runs, only ever touches work.hda (never golden.hda), finds the Apple_HFS partition by
# reading the Apple partition map, uses `hcopy -m` so the MacBinary type/creator and
# both forks survive, prints `hls -l` of the destination, and always humounts (trap). A
# missing destination folder is created with hmkdir (one level deep).
set -euo pipefail
MP_HOME="${MP_HOME:-$HOME/.local/share/moofport}"
export MP_HOME   # snow.sh (the running check below) must see this one, not one under the HOME set further down
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${MP_DISK:-$MP_HOME/disks/work.hda}"   # MP_DISK: another image under $MP_HOME (snow.sh)
case "$(realpath "$WORK")" in "$(realpath "$MP_HOME")"/*) ;; *) echo "deploy.sh: MP_DISK must be under $MP_HOME" >&2; exit 1;; esac

die() { echo "deploy.sh: $*" >&2; exit 1; }

command -v hmount >/dev/null || die "hfsutils not found; install hfsutils first"

# hfsutils keeps its current volume in $HOME/.hcwd. A MoofTCP session may be deploying at the
# same moment and both disks are named MoofHD, so ours gets its own state.
mkdir -p "$MP_HOME"
export HOME="$MP_HOME"

files=()
dest=""
while [ $# -gt 0 ]; do
  case "$1" in
    --to) [ $# -ge 2 ] || die "--to needs a folder like ':MoofTCP Test:'"; dest="$2"; shift 2;;
    *) files+=("$1"); shift;;
  esac
done
[ "${#files[@]}" -ge 1 ] || die "usage: deploy.sh <file.bin>... --to ':Folder:'"
[ -n "$dest" ] || die "missing --to ':Folder:'"
case "$dest" in :*:) ;; *) die "--to must be an HFS path like ':MoofTCP Test:'";; esac
for f in "${files[@]}"; do
  [ -f "$f" ] || die "no such file: $f"
  case "$f" in *.bin) ;; *) die "$f is not a MacBinary .bin file";; esac
done
[ -f "$WORK" ] || die "missing $WORK (run scripts/fresh-disk.sh)"

if "$ROOT/scripts/snow.sh" status >/dev/null 2>&1; then
  die "Snow is running; stop it first (scripts/snow.sh stop)"
fi

# Which Apple_HFS partition (hfsutils counts them from 1)? Read the Apple partition map.
part="$(python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read(512 * 64)
n = 0
for i in range(1, 64):
    b = d[512*i:512*i+512]
    if b[:2] != b"PM":
        break
    if b[48:80].split(b"\0")[0] == b"Apple_HFS":
        n += 1
        print(n)
        break
' "$WORK")"
[ -n "$part" ] || die "no Apple_HFS partition in $WORK"

vol="$(hmount "$WORK" "$part" | sed -n 's/^Volume name is "\(.*\)"$/\1/p')"
[ -n "$vol" ] || die "hmount failed"
# After humount, clear the "cleanly unmounted" bit hfsutils sets (sysdrvr.py clear_unmounted_bit):
# on the real-Classic image Snow otherwise hangs at "Welcome to Macintosh" on the next boot.
cleanup() {
  humount "$vol:" >/dev/null 2>&1 || true
  python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import sysdrvr; sysdrvr.clear_unmounted_bit(sys.argv[2])' \
    "$ROOT/scripts" "$WORK" || echo "deploy.sh: could not clear the unmounted bit" >&2
}
trap cleanup EXIT

# Create the destination folder if it is missing (one level). hcd, not hls: hls exits 0 for
# a missing path, and hcopy then writes every file over a plain file with the folder's name.
if ! hcd "$dest" >/dev/null 2>&1; then
  hmkdir "$dest" || die "could not create $dest"
fi
hcd "$vol:" >/dev/null   # back to the volume root (":" alone is the current folder)

for f in "${files[@]}"; do
  hcopy -m "$f" "$dest" || die "hcopy failed for $f"
done
hls -l "$dest"
echo "deployed ${#files[@]} file(s) to $vol$dest"

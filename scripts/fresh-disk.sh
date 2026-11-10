#!/usr/bin/env bash
# fresh-disk.sh - make a fresh writable work disk from MoofTCP's golden image.
#
# MoofPort reuses the sibling MoofTCP project's golden System 6.0.8 image
# (System 6.0.8 + DaynaPORT SCSI/Link 1.2.5 + MacTCP 2.1, 10.0.0.2/8). That image is
# built and owned by MoofTCP (docs/dev/GOLDEN-IMAGE.md there); MoofPort never builds or
# mounts it, only copies it read-only to its own work.hda.
#
#   source:  $MT_HOME/disks/golden.hda   (MT_HOME default ~/.local/share/mooftcp)
#   dest:    $MP_HOME/disks/work.hda      (MP_HOME default ~/.local/share/moofport)
#
# If tools/golden.sha256 exists it is treated as the expected SHA-256 of golden.hda and
# the copy is refused on a mismatch (copy MoofTCP's tools/golden.sha256 here
# once the golden image is settled; MoofTCP's fresh-disk.sh hash-checks the same way).
# The golden image is read only; work.hda is made writable. Never run while Snow runs
# (snow.sh/deploy.sh guard work.hda); this only writes work.hda, never golden.hda.
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
MP_HOME="${MP_HOME:-$HOME/.local/share/moofport}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GOLDEN="$MT_HOME/disks/golden.hda"
WORK="$MP_HOME/disks/work.hda"
HASH_FILE="$ROOT/tools/golden.sha256"

die() { echo "fresh-disk.sh: $*" >&2; exit 1; }

[ -f "$GOLDEN" ] || die "missing golden image $GOLDEN (build it in the MoofTCP project; see its docs/dev/GOLDEN-IMAGE.md)"

# Refuse if a MoofPort Snow is running (it would be using work.hda).
if "$ROOT/scripts/snow.sh" status >/dev/null 2>&1; then
  die "Snow is running; stop it first (scripts/snow.sh stop)"
fi

if [ -f "$HASH_FILE" ]; then
  want="$(grep -Eo '[0-9a-f]{64}' "$HASH_FILE" | head -n1 || true)"
  [ -n "$want" ] || die "no sha256 in $HASH_FILE"
  got="$(sha256sum "$GOLDEN" | cut -d' ' -f1)"
  [ "$want" = "$got" ] || die "golden hash mismatch: expected $want, got $got"
  echo "golden hash OK ($got)"
else
  echo "fresh-disk.sh: note: no $HASH_FILE; copying golden image unchecked" >&2
fi

mkdir -p "$MP_HOME/disks"
rm -f "$WORK"
cp "$GOLDEN" "$WORK"
chmod u+w "$WORK"
echo "wrote $WORK ($(stat -c%s "$WORK") bytes)"

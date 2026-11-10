#!/usr/bin/env bash
# phase0-run.sh - build the Phase 0 app kit, put it on a fresh work disk, start the host
# test services and boot Snow, ready for the Phase 0 ground-truth runs (docs/dev/PLAN.md section 4).
#
#   phase0-run.sh [--no-spy] [--no-testnet] [--keep-disk]
#     --no-spy      do not install SpyENET into the System Folder (clean boot)
#     --no-testnet  do not start scripts/testnet.py
#     --keep-disk   use the current work.hda instead of a fresh copy
#
# The apps (EnetBench, TcpBench, CpuMeter, HeapInfo, HeapInfoOpen, SpyDump, ScsiProbe) go
# into ":MoofPort Test:" on work.hda; SpyENET, which is an INIT, goes into
# ":System Folder:" (unless --no-spy). Each app writes its result to a text file on the
# boot volume; after a run, stop Snow and pull the files with hfsutils (deploy.sh's mount
# helper, or hmount/hcopy directly). This script boots Snow and leaves it running; it does
# not drive the GUI (the apps are launched by hand or by a separate automation step).
set -euo pipefail
MP_HOME="${MP_HOME:-$HOME/.local/share/moofport}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "phase0-run.sh: $*" >&2; exit 1; }

install_spy=1; start_testnet=1; keepdisk=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-spy) install_spy=""; shift;;
    --no-testnet) start_testnet=""; shift;;
    --keep-disk) keepdisk=1; shift;;
    *) die "usage: phase0-run.sh [--no-spy] [--no-testnet] [--keep-disk]";;
  esac
done

APPS="EnetBench TcpBench CpuMeter HeapInfo HeapInfoOpen SpyDump ScsiProbe"

cd "$ROOT"
echo "== build apps"
for a in $APPS; do
  echo "-- $a"
  scripts/build-mac.sh "$a" >/dev/null
done
if [ -n "$install_spy" ]; then
  echo "-- SpyENET"
  scripts/build-mac.sh SpyENET >/dev/null
fi

# Locate each built MacBinary (build-mac/<lowercased dir>/<Name>.bin).
find_bin() {
  local name="$1" f
  f="$(find build-mac -name "$name.bin" -type f 2>/dev/null | head -n1)"
  [ -n "$f" ] || die "build did not produce $name.bin"
  echo "$f"
}

echo "== disk"
scripts/snow.sh stop >/dev/null 2>&1 || true
[ -n "$keepdisk" ] || scripts/fresh-disk.sh

bins=()
for a in $APPS; do bins+=("$(find_bin "$a")"); done
scripts/deploy.sh "${bins[@]}" --to ":MoofPort Test:"
if [ -n "$install_spy" ]; then
  scripts/deploy.sh "$(find_bin SpyENET)" --to ":System Folder:"
fi

started_testnet=""
if [ -n "$start_testnet" ]; then
  if ! (exec 3<>/dev/tcp/127.0.0.1/17007) 2>/dev/null; then
    mkdir -p "$MP_HOME"
    scripts/testnet.py > "$MP_HOME/testnet.log" 2>&1 &
    started_testnet=$!
    echo "started testnet.py (pid $started_testnet, log $MP_HOME/testnet.log)"
    sleep 1
  else
    echo "testnet.py already listening on 17007; not starting another"
  fi
fi

echo "== boot"
scripts/snow.sh start
echo
echo "Snow is running. Launch the apps from ':MoofPort Test:' in the Finder."
echo "Each writes its results to a text file on the boot volume:"
echo "  EnetBench -> EnetBench.out   TcpBench -> TcpBench.out   CpuMeter -> (on screen)"
echo "  ScsiProbe -> ScsiProbe.out   SpyDump  -> SpyENET.log    HeapInfo/HeapInfoOpen -> alert + text"
echo "When done: scripts/snow.sh stop, then pull the files with hfsutils."
if [ -n "$started_testnet" ]; then
  echo "testnet.py pid $started_testnet is still running; kill it when finished."
fi

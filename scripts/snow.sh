#!/usr/bin/env bash
# Control Snow on a private Xvfb display. Usage:
#   snow.sh start [--floppy <img>]   render workspace, start Xvfb (if needed) and Snow
#   snow.sh stop                     kill Snow (and our Xvfb) by recorded PID
#   snow.sh status                   exit 0 running / 1 stopped
#   snow.sh shot <png>               screenshot the whole Xvfb screen
#
# Port of MoofTCP's scripts/snow.sh for MoofPort. Differences:
#   - home is $MP_HOME (default ~/.local/share/moofport), separate from MoofTCP's. The
#     pinned Snow and the ROM are MoofTCP's, read-only from $MT_HOME.
#   - the private display range is :140-:149, DEFAULT :143 (MoofTCP defaults to :142),
#     so a MoofPort Snow does not collide with a MoofTCP Snow running on :142. Only our own
#     Snow is checked; note MoofTCP's snow.sh still refuses to start while ours runs.
#   - PIDs and the log live under $MP_HOME.
# MP_ETH_LINK overrides the Ethernet link type of the workspace (default NAT; Down = cable unplugged;
# tap:<dev> bridges to a host TAP device, which is created with sudo).
# MP_DISK boots another image under $MP_HOME instead of disks/work.hda (for example the EtherTalk test
# image); MP_PRAM gives Snow a PRAM file, which it memory-maps and writes (the AppleTalk connection
# choice lives in PRAM), so pass a copy.
# MP_NO_ETH=1 removes the DaynaPORT from the SCSI bus (no-device boot test).
# MP_DISPLAY overrides the display (default :143). Only :140-:149 are accepted: :0/:1 are
# the desktop, and other displays (:97, :99, ...) belong to other sessions.
set -euo pipefail
MP_HOME="${MP_HOME:-$HOME/.local/share/moofport}"
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MP_DISPLAY="${MP_DISPLAY:-:143}"
SNOW_PID_FILE="$MP_HOME/snow.pid"
XVFB_PID_FILE="$MP_HOME/xvfb.pid"
WS="$MP_HOME/moofport.snoww"

die() { echo "snow.sh: $*" >&2; exit 1; }
# Normalize "[host]:N[.screen]" to ":N", then allow only our private range.
DISP_NUM="${MP_DISPLAY##*:}"; DISP_NUM="${DISP_NUM%%.*}"
[[ "$MP_DISPLAY" == *:* && "$DISP_NUM" =~ ^[0-9]+$ ]] || die "bad display '$MP_DISPLAY' (want :140-:149)"
DISP_NUM=$((10#$DISP_NUM))
{ [ "$DISP_NUM" -ge 140 ] && [ "$DISP_NUM" -le 149 ]; } \
  || die "refusing display $MP_DISPLAY (only :140-:149 are ours; :0/:1 desktop, :97/:99 other sessions)"
MP_DISPLAY=":$DISP_NUM"
need() { command -v "$1" >/dev/null || die "$1 not found; install $2 first (see docs)"; }

alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
readpid() { if [ -f "$1" ]; then tr -d '[:space:]' < "$1"; fi; }

snow_running() {
  local p; p="$(readpid "$SNOW_PID_FILE")"
  alive "$p" && [ "$(cat "/proc/$p/comm" 2>/dev/null)" = snowemu ]
}

# A MoofPort Snow is one running our workspace. MoofTCP's runs side by side on :142, with its
# own disk and workspace; this is intended.
ours_running() { pgrep -fx ".*/snowemu $WS.*" >/dev/null; }

cmd_status() {
  if snow_running; then echo "running (pid $(readpid "$SNOW_PID_FILE"))"; exit 0; fi
  echo "stopped"; exit 1
}

cmd_start() {
  local floppy=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --floppy)
        [ $# -ge 2 ] || die "--floppy needs an image"
        [ -f "$2" ] || die "no such floppy: $2"
        floppy=(--floppy "$2"); shift 2;;
      *) die "unknown start option: $1";;
    esac
  done
  if snow_running; then die "already running (pid $(readpid "$SNOW_PID_FILE"))"; fi
  if ours_running; then die "a MoofPort snowemu we have no PID for is running; refusing"; fi
  need Xvfb xvfb; need xdpyinfo x11-utils
  [ -x "$MT_HOME/snow/snowemu" ] || die "missing $MT_HOME/snow/snowemu (MoofTCP's scripts/fetch-snow.sh)"
  local disk="${MP_DISK:-$MP_HOME/disks/work.hda}" pram="" link="\"${MP_ETH_LINK:-NAT}\""
  case "${MP_ETH_LINK:-}" in
    tap:*)   # Snow's TapBridge: frames go straight to a host TAP device (AppleTalk, inbound)
      local tdev="${MP_ETH_LINK#tap:}"
      [[ "$tdev" =~ ^(tap|snow)[A-Za-z0-9_]{0,10}$ ]] || die "TAP device name must start with tap or snow (Snow's rule)"
      ip link show "$tdev" >/dev/null 2>&1 || die "no TAP device $tdev (run: sudo ip tuntap add dev $tdev mode tap user $USER && sudo ip link set $tdev up)"
      link="{\"TapBridge\": \"$tdev\"}";;
  esac
  [ -f "$disk" ] || die "missing $disk (run scripts/fresh-disk.sh)"
  case "$(realpath "$disk")" in "$(realpath "$MP_HOME")"/*) ;; *) die "MP_DISK must be under $MP_HOME";; esac
  if [ -n "${MP_PRAM:-}" ]; then
    [ -f "$MP_PRAM" ] || die "no such PRAM file: $MP_PRAM"
    pram="\n  \"pram_path\": \"$(realpath "$MP_PRAM")\","
  fi
  mkdir -p "$MP_HOME"
  sed -e "s#@MP_HOME@#$MP_HOME#g" -e "s#@MT_HOME@#$MT_HOME#g" -e "s#@DISK@#$disk#g" -e "s#@PRAM@#$pram#g" -e "s#\"@ETH_LINK@\"#$link#g" "$ROOT/tools/moofport.snoww.in" > "$WS"
  # MP_NO_ETH=1: no DaynaPORT on the bus (compatibility test: MoofPort must boot without its device)
  if [ "${MP_NO_ETH:-}" = 1 ]; then sed -i 's/"Ethernet"/"None"/' "$WS"; fi

  if ! xdpyinfo -display "$MP_DISPLAY" >/dev/null 2>&1; then
    Xvfb "$MP_DISPLAY" -screen 0 1280x1024x24 -nolisten tcp >/dev/null 2>&1 &
    echo $! > "$XVFB_PID_FILE"
    local ok=
    for _ in $(seq 50); do
      if xdpyinfo -display "$MP_DISPLAY" >/dev/null 2>&1; then ok=1; break; fi
      sleep 0.1
    done
    [ -n "$ok" ] || die "Xvfb $MP_DISPLAY did not come up"
  fi
  env -u WAYLAND_DISPLAY DISPLAY="$MP_DISPLAY" "$MT_HOME/snow/snowemu" "$WS" "${floppy[@]}" \
      >> "$MP_HOME/snow.log" 2>&1 &
  echo $! > "$SNOW_PID_FILE"
  sleep 1
  snow_running || die "Snow exited immediately; see $MP_HOME/snow.log"
  echo "started (pid $(readpid "$SNOW_PID_FILE"), display $MP_DISPLAY)"
}

cmd_stop() {
  local p; p="$(readpid "$SNOW_PID_FILE")"
  if snow_running; then
    kill "$p"
    for _ in $(seq 50); do alive "$p" || break; sleep 0.1; done
    if alive "$p"; then kill -9 "$p" 2>/dev/null || true; sleep 0.5; fi
  fi
  rm -f "$SNOW_PID_FILE"
  if ours_running; then die "snowemu still running after stop"; fi
  local x; x="$(readpid "$XVFB_PID_FILE")"
  # Kill the recorded Xvfb only if it is still ours: comm is Xvfb AND its argv holds our
  # display (a stale PID may have been reused, e.g. by another session's Xvfb :99).
  if alive "$x" && [ "$(cat "/proc/$x/comm" 2>/dev/null)" = Xvfb ] \
     && tr '\0' '\n' < "/proc/$x/cmdline" 2>/dev/null | grep -qx -- "$MP_DISPLAY"; then
    kill "$x"
    for _ in $(seq 30); do alive "$x" || break; sleep 0.1; done
    if alive "$x"; then die "Xvfb $MP_DISPLAY (pid $x) did not exit"; fi
  elif alive "$x"; then
    echo "snow.sh: recorded pid $x is not Xvfb $MP_DISPLAY; leaving it alone" >&2
  fi
  rm -f "$XVFB_PID_FILE"
  echo "snow stopped"
}

cmd_shot() {
  [ $# -eq 1 ] || die "usage: snow.sh shot <png>"
  DISPLAY="$MP_DISPLAY" scrot -o "$1"
  echo "$1"
}

sub="${1:-}"
if [ $# -gt 0 ]; then shift; fi
case "$sub" in
  start) cmd_start "$@";;
  stop) cmd_stop;;
  status) cmd_status;;
  shot) cmd_shot "$@";;
  *) die "usage: snow.sh start [--floppy img] | stop | status | shot <png>";;
esac

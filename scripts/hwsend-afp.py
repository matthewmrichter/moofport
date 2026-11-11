#!/usr/bin/env python3
"""hwsend-afp.py - time an AfpCopy run from a host capture (AppleShare over EtherTalk).

    scripts/hwsend-afp.py <capture.pcap> --ether 00:80:19:xx:xx:xx [--bytes 344064]

AfpCopy (mac/afpcopy) copies one file (by default 320 KB data + 16 KB resource fork = 344,064
bytes) to the server and back, and times it with the Mac's Ticks, which MoofPort's long
interrupt-level stretches undercount (hwsend3). This times it on the PC instead, from a capture
of every frame to and from the Mac's Ethernet address (scripts/hwsend-host.sh --afp):

  to server    the span of the Mac's large AppleTalk frames (ASP write data, >= 500 bytes)
  from server  the span of the server's large AppleTalk frames to the Mac

and gives bytes / span, the frame counts, and stalls (no large frame in that direction for 0.5 s
or more: an ATP retry). The two phases follow each other, so they don't overlap.
"""
import argparse
import re
import subprocess
import sys

LINE = re.compile(r"^(\d+\.\d+) ([0-9a-f:]{17}) > ([0-9a-f:]{17}), 802\.3, length (\d+):")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("capture")
    ap.add_argument("--ether", required=True, help="the Mac's Ethernet address")
    ap.add_argument("--bytes", type=int, default=344064, help="bytes AfpCopy moves each way")
    ap.add_argument("--min", type=int, default=500, help="smallest frame counted as data")
    a = ap.parse_args()
    mac = a.ether.lower()
    r = subprocess.run(["tcpdump", "-r", a.capture, "-n", "-e", "-tt"], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("hwsend-afp.py: tcpdump -r failed: %s" % r.stderr.strip())
    up, down, small = [], [], 0
    for ln in r.stdout.splitlines():
        if "appletalk" not in ln.lower():
            continue
        m = LINE.match(ln)
        if not m:
            continue
        t, src, dst, n = float(m.group(1)), m.group(2), m.group(3), int(m.group(4))
        if n < a.min:
            small += 1
        elif src == mac:
            up.append((t, n))
        elif dst == mac:
            down.append((t, n))
    print("hwsend-afp: %s, Mac %s: %d large frames to the server, %d from it, %d small"
          % (a.capture, mac, len(up), len(down), small))
    for name, fr in (("to server", up), ("from server", down)):
        if len(fr) < 2:
            print("  %-12s no data" % name)
            continue
        span = fr[-1][0] - fr[0][0]
        stalls = sum(1 for i in range(1, len(fr)) if fr[i][0] - fr[i - 1][0] >= 0.5)
        print("  %-12s %d frames, %d bytes on the wire, %.2f s: %.0f B/s of file data; "
              "stalls of 0.5 s or more: %d" % (name, len(fr), sum(n for _, n in fr), span,
                                               a.bytes / span if span > 0 else 0, stalls))


if __name__ == "__main__":
    main()

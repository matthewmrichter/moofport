#!/usr/bin/env python3
"""csumwatch.py - log the host kernel's checksum-error counters while the Mac sends (hw5).

    scripts/csumwatch.py [interval_s] >> ~/.local/share/moofport/csumwatch.log &

Blind SCSI writes (MoofPort's "txblind") could damage frame bytes between the Mac's memory and
the BlueSCSI. The BlueSCSI then sends the damaged frame over Wi-Fi with a correct Wi-Fi checksum,
so the only place it shows is the TCP/UDP checksum here on the receiving PC: the kernel drops
the segment and counts it. This prints, every interval (default 10 s), the change in
Tcp InCsumErrors, Udp InCsumErrors and Ip InHdrErrors (/proc/net/snmp) with a timestamp, and a
line only when something changed or once a minute. Read-only; needs no privileges.
"""
import sys
import time

KEYS = [("Ip", "InHdrErrors"), ("Tcp", "InCsumErrors"), ("Udp", "InCsumErrors")]


def snap():
    rows = {}
    lines = open("/proc/net/snmp").read().splitlines()
    for i in range(0, len(lines) - 1, 2):
        names, vals = lines[i].split(), lines[i + 1].split()
        rows[names[0].rstrip(":")] = dict(zip(names[1:], vals[1:]))
    return {k: int(rows.get(k[0], {}).get(k[1], 0)) for k in KEYS}


def main():
    interval = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    last = snap()
    print("%s csumwatch start %s" % (time.strftime("%F %T"),
          " ".join("%s.%s=%d" % (k[0], k[1], last[k]) for k in KEYS)), flush=True)
    quiet = 0.0
    while True:
        time.sleep(interval)
        now = snap()
        d = {k: now[k] - last[k] for k in KEYS}
        quiet += interval
        if any(d.values()) or quiet >= 60:
            print("%s %s" % (time.strftime("%F %T"),
                  " ".join("%s.%s +%d" % (k[0], k[1], d[k]) for k in KEYS)), flush=True)
            quiet = 0.0
        last = now


if __name__ == "__main__":
    main()

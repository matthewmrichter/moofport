#!/usr/bin/env python3
"""testnet-stalls.py - per-test stalls (>= 0.5 s gaps) from testnet.py's log, split by MoofBatch markers.

    testnet-stalls.py <testnet.log> [--run PREFIX]...

testnet.py logs a "stalls N (...)" tail on each echo, bulk sink and bulk source line (hwsend
auto1). Each such line belongs to the MoofBatch test whose "MARK <run> <setting> <n> <test> start"
came last. With the Mac's one-at-a-time sends, one stall is one segment lost after the
BlueSCSI took it (MacTCP re-sends after ~1 s); in receive a stall is a lost Mac ACK or a lost
data segment the PC re-sent. Prints each test, then totals per setting with frames estimated
from the test (A/C: 191 data segments, E: 64).
"""
import argparse
import collections
import math
import re
import sys

MARK = re.compile(r"MARK (\S+) (\S+) (\d+) ([A-Z]) (start|end)")
RES = re.compile(r"TCP (\d+) (echoed|bulk sink received|bulk source sent) (\d+) bytes(?: in ([\d.]+)s)?.*?; (?:.*; )?stalls (\d+) \(([\d.]+) s")
FRAMES = {"A": 191, "C": 191, "E": 64}


def ci(k, n):
    """Rough 95% interval for k events in n (per 1,000), Poisson."""
    if n == 0:
        return (0, 0)
    lo = 0.0 if k == 0 else 0.5 * _chi2inv(0.025, 2 * k)
    hi = 0.5 * _chi2inv(0.975, 2 * k + 2)
    return (1000 * lo / n, 1000 * hi / n)


def _chi2inv(p, df):
    # Wilson-Hilferty approximation, good enough for a rough interval
    z = {0.025: -1.959964, 0.975: 1.959964}[p]
    return df * (1 - 2 / (9 * df) + z * math.sqrt(2 / (9 * df))) ** 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--run", action="append", default=[], help="only runs whose id starts with this")
    a = ap.parse_args()
    cur = None
    rows = []
    for ln in open(a.log, errors="replace"):
        m = MARK.search(ln)
        if m:
            run, setting, n, test, what = m.groups()
            cur = (run, setting, int(n), test) if what == "start" else cur
            continue
        m = RES.search(ln)
        if m and cur:
            if a.run and not any(cur[0].startswith(r) for r in a.run):
                continue
            port, kind, nbytes, secs, stalls, ssum = m.groups()
            rows.append(cur + (float(secs) if secs else None, int(stalls), float(ssum)))
            cur = None
    if not rows:
        sys.exit("testnet-stalls.py: no test lines with stall counts")
    print("%-8s %-10s %-4s %8s %7s" % ("run", "setting", "test", "seconds", "stalls"))
    tot = collections.defaultdict(lambda: [0, 0])
    for run, setting, n, test, secs, stalls, ssum in rows:
        print("%-8s %-10s %s#%d %8s %7d" % (run, setting, test, n, "%.2f" % secs if secs else "-", stalls))
        if test in FRAMES:
            t = tot[setting]
            t[0] += stalls
            t[1] += FRAMES[test]
    print("\nper setting, sends (A, C, E; frames estimated): stalls / frames = per 1,000 (rough 95% CI)")
    for setting, (k, n) in tot.items():
        lo, hi = ci(k, n)
        print("  %-10s %4d / %5d = %5.1f  (%.1f-%.1f)" % (setting, k, n, 1000 * k / n, lo, hi))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""hwsend-batch.py - split a MoofBatch capture by its test markers and summarise each test.

    hwsend-batch.py <capture.pcap> [--mac IP]

MoofBatch (mac/tcpsend, built with MP_BATCH) sends a UDP datagram to the PC's port 17040 before
and after every test: "MARK <run> <setting> <n> <test> start" / "... end", test one of E A B C R T.
hwsend-host.sh captures them with the TCP traffic. Each TCP connection that starts between a
start and its end belongs to that test. Per test this prints the PC-timed figures of
hwsend-acks.py (throughput, Mac turnaround, LOST), then a table per setting.
"""
import argparse
import importlib.util
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("acks", os.path.join(HERE, "hwsend-acks.py"))
acks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acks)

MARK = re.compile(r"MARK (\S+) (\S+) (\d+) ([A-Z]) (start|end)")


def markers(cap):
    r = subprocess.run(["tcpdump", "-r", cap, "-tt", "-n", "-A", "udp port 17040"],
                       capture_output=True, text=True)
    out, t = [], None
    for ln in r.stdout.splitlines():
        m = re.match(r"^(\d+\.\d+) IP ", ln)
        if m:
            t = float(m.group(1))
            continue
        m = MARK.search(ln)
        if m and t is not None:
            out.append((t,) + m.groups())
            t = None
    return out


def term_rounds(pk):
    """T: per round, the Mac's `k` to the last host data segment before its next `k`; one line."""
    rounds, t0, last = [], None, None
    for p in pk:
        if p["frm"] == "mac" and p["len"] > 0:
            if t0 is not None and last is not None:
                rounds.append(last - t0)
            t0, last = p["t"], None
        elif p["frm"] == "host" and p["len"] > 0 and t0 is not None:
            last = p["t"]
    if t0 is not None and last is not None:
        rounds.append(last - t0)
    if not rounds:
        return "  term rounds   none"
    ms = sorted(round(x * 1000, 3) for x in rounds)
    n = len(ms)
    return "  term rounds   n %4d  min %7.2f  med %7.2f  p90 %7.2f  max %8.2f ms (Mac k to last host segment)" % (
        n, ms[0], ms[n // 2], ms[min(n - 1, int(n * 0.9))], ms[-1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--mac", default="192.168.50.201")
    a = ap.parse_args()
    mk = markers(a.capture)
    if not mk:
        sys.exit("hwsend-batch.py: no MARK datagrams in %s" % a.capture)
    r = subprocess.run(["tcpdump", "-r", a.capture, "-tt", "-n", "-S", "tcp"], capture_output=True, text=True)
    order, conns = acks.parse(r.stdout.splitlines(), a.mac)
    # Markers are single UDP datagrams and can be lost like any frame (hwsend11: 1 in 48). A test
    # whose end marker is missing ends at the next start marker; a connection that falls after a
    # test's end and before the next start (its own start marker lost) is listed as unlabelled.
    starts = [(t, s, n, test) for t, run, s, n, test, what in mk if what == "start"]
    ends = {(s, n, test): t for t, run, s, n, test, what in mk if what == "end"}
    tests = []                      # (setting, n, test, t0, t1)
    for i, (t, setting, n, test) in enumerate(starts):
        t1 = ends.get((setting, n, test))
        if t1 is None:
            t1 = starts[i + 1][0] if i + 1 < len(starts) else float("inf")
        tests.append((setting, int(n), test, t, t1))
    # An end marker whose start was lost: the test began after the marker before it.
    have = {(s, str(n), test) for s, n, test, _, _ in tests}
    times = sorted(t for t, *_ in mk)
    for (setting, n, test), t1 in ends.items():
        if (setting, n, test) not in have:
            t0 = max([t for t in times if t < t1], default=t1 - 60)
            tests.append((setting, int(n), test, t0, t1))
    tests.sort(key=lambda x: x[3])
    inside = set()
    for setting, n, test, t0, t1 in tests:
        inside.update(k for k in order if t0 <= conns[k][0]["t"] <= t1)
    stray = [k for k in order if k not in inside and k[1] in (17031, 17007, 17030, 17032)]
    print("hwsend batch summary: %s, Mac %s, run %s, %d tests; markers %d of %d (each test sends 2)"
          % (a.capture, a.mac, mk[0][1], len(tests), len(mk), 2 * len(tests)))
    if stray:
        print("unlabelled connections (start marker lost): %s" % ", ".join(
            "%d->%d at %.1f s" % (k[0], k[1], conns[k][0]["t"] - mk[0][0]) for k in stray))
    table = {}
    total_lost = 0
    for setting, n, test, t0, t1 in tests:
        keys = [k for k in order if t0 <= conns[k][0]["t"] <= t1]
        print("\n== %s %s#%d (%.1f s)" % (setting, test, n, t1 - t0))
        row = table.setdefault(setting, {})
        for k in keys:
            pk = conns[k]
            if k[1] in (17031, 17007):
                lost, rep = acks.pauses(pk)
                total_lost += lost
                print("  lost          %d after a good WRITE, %d repeats" % (lost, rep))
                row.setdefault("lost", 0)
                row["lost"] += lost
            if k[1] == 17031:
                lines = acks.sink(pk)
                print("\n".join(lines))
                m = re.search(r"(\d+) B/s", lines[0])
                if m:
                    row.setdefault(test, []).append(int(m.group(1)))
                turns = [l for l in lines if "mac turn" in l and "med" in l]
                if turns:
                    row.setdefault(test + "turn", []).append(float(turns[0].split("med")[1].split()[0]))
            elif k[1] == 17007:
                lines = acks.echo(pk)
                print("\n".join(lines))
                turns = [l for l in lines if "mac turn" in l and "med" in l]
                if turns:
                    row.setdefault("Eturn", []).append(float(turns[0].split("med")[1].split()[0]))
            elif k[1] == 17032:
                # T (terminal): the Mac's 1-byte `k`, then 25 small host segments. The host
                # segments, bytes, retransmissions and stalls are those of acks.recv; the round
                # time (k to the last segment of its answer) is the PC-clock figure.
                lines = acks.recv(pk) + [term_rounds(pk)]
                print("\n".join(lines))
                lost, rep = acks.pauses(pk)
                total_lost += lost
                print("  lost          %d after a good WRITE, %d repeats" % (lost, rep))
                row.setdefault("lost", 0)
                row["lost"] += lost
            elif k[1] == 17030:
                lines = acks.recv(pk)
                print("\n".join(lines))
                m = re.search(r"(\d+) B/s", lines[0])
                if m:
                    row.setdefault("R", []).append(int(m.group(1)))
    print("\nLOST: %d segments lost after a good WRITE in this capture" % total_lost)
    print("\nper setting (KB/s, each run; turnaround = median ms; lost = segments lost after a good WRITE)")
    print("%-10s %-14s %-14s %-14s %-12s %-12s %-10s %s" % ("setting", "receive", "A", "C", "A turn",
                                                             "C turn", "echo", "lost"))
    f = lambda v: "/".join("%.1f" % (x / 1000.0) for x in v) if v else "-"
    g = lambda v: "/".join("%.1f" % x for x in v) if v else "-"
    for setting, row in table.items():
        print("%-10s %-14s %-14s %-14s %-12s %-12s %-10s %d" % (
            setting, f(row.get("R")), f(row.get("A")), f(row.get("C")), g(row.get("Aturn")),
            g(row.get("Cturn")), g(row.get("Eturn")), row.get("lost", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""hwsend-acks.py - summarise a TcpSend capture: ACK delays and who waits for whom.

    scripts/hwsend-acks.py <capture.pcap> [--mac 192.168.50.201] [--modes EABCN]
    scripts/hwsend-acks.py --text <tcpdump -tt -n -S output> [...]

Part of the hwsend kit (docs/dev/hwsend.md); scripts/hwsend-host.sh runs it after a capture.
Reads the capture with `tcpdump -r -tt -n -S` (no root needed for a file) and, for every TCP
connection from the Mac to the test host, in the order they started:

  bulk sink (17031), TcpSend modes A, B, C, N in the order given by --modes:
    ack delay     a host ACK that acknowledges new data, minus the arrival of the newest Mac
                  segment it covers: how long Linux held the ACK (delayed ACK shows as ~40 ms)
    segs per ack  how many Mac segments one ACK covered
    mac turn      a Mac data segment that follows a host ACK (with no Mac data in between),
                  minus that ACK: the Mac's side of the round trip (driver poll latency, MacTCP,
                  the SCSI WRITE)
    seg gap       Mac data segment to the next one
    lost          stalls (no Mac data for 0.5 s or more) and holes (a later segment before an
                  earlier one, which came ~1 s later: MacTCP's retransmission of a lost frame)
  bulk source (17030), mode R (receive):
    host data, retransmitted segments (host resent: a Mac ACK or a host frame was lost), the
    Mac's ACK delay (host data segment -> the Mac ACK that covers it), stalls of 0.5 s or more
  echo (17007), mode E:
    host echo     a Mac data byte to the host's echo of it
    mac turn      the host's echo to the Mac's next data byte (Mac side of the echo RTT)
    mac ack       the host's echo to the Mac's pure ACK of it, if the Mac sent one

All times are host-side capture times (one clock, no Mac clock involved). Each figure is given as
count, min / median / mean / p90 / max ms, and a histogram.
"""
import argparse
import re
import subprocess
import sys

BINS = [0, 1, 2, 5, 10, 20, 30, 40, 50, 60, 100, 200, 500]
LINE = re.compile(r"^(\d+\.\d+) IP (\d+\.\d+\.\d+\.\d+)\.(\d+) > (\d+\.\d+\.\d+\.\d+)\.(\d+): "
                  r"Flags \[([^\]]*)\](.*)$")
SEQ = re.compile(r"seq (\d+)(?::(\d+))?")
ACK = re.compile(r"ack (\d+)")
LEN = re.compile(r"length (\d+)")


def parse(lines, mac):
    """Packets as dicts, grouped by connection (Mac port, host port) in order of appearance."""
    conns = {}
    order = []
    cur = {}                        # (Mac port, host port) -> its current connection key
    for ln in lines:
        m = LINE.match(ln.strip())
        if not m:
            continue
        t, sa, sp, da, dp, flags, rest = m.groups()
        if sa == mac:
            key, frm = (int(sp), int(dp)), "mac"
        elif da == mac:
            key, frm = (int(dp), int(sp)), "host"
        else:
            continue
        sm, am, lm = SEQ.search(rest), ACK.search(rest), LEN.search(rest)
        n = int(lm.group(1)) if lm else 0
        p = dict(t=float(t), frm=frm, flags=flags, len=n,
                 seq=int(sm.group(1)) if sm else None,
                 end=int(sm.group(2)) if sm and sm.group(2) else None,
                 ack=int(am.group(1)) if am else None)
        if frm == "mac" and flags == "S" and key in cur and conns[cur[key]]:
            del cur[key]            # MacTCP reused the port: a new connection (hwsend7 i7)
        if key not in cur:
            cur[key] = key + (sum(1 for k in order if k[:2] == key),)
            conns[cur[key]] = []
            order.append(cur[key])
        conns[cur[key]].append(p)
    return order, conns


def stats(name, v):
    v = sorted(v)
    if not v:
        return "  %-14s none" % name
    n = len(v)
    ms = [round(x * 1000, 3) for x in v]
    h = [0] * len(BINS)
    for x in ms:
        i = max([0] + [i for i, b in enumerate(BINS) if x >= b])   # negative (reordered): bin 0
        h[i] += 1
    hist = " ".join("%d-%s:%d" % (BINS[i], BINS[i + 1] if i + 1 < len(BINS) else "", c)
                    for i, c in enumerate(h) if c)
    return ("  %-14s n %4d  min %7.2f  med %7.2f  mean %7.2f  p90 %7.2f  max %8.2f ms\n"
            "  %-14s ms bins: %s" % (name, n, ms[0], ms[n // 2], sum(ms) / n,
                                    ms[min(n - 1, int(n * 0.9))], ms[-1], "", hist))


def sink(pk):
    data = [p for p in pk if p["frm"] == "mac" and p["len"] > 0]
    out = []
    if not data:
        return ["  no data from the Mac"]
    t0, t1 = data[0]["t"], data[-1]["t"]
    total = sum(p["len"] for p in data)
    out.append("  %d data segments, %d bytes, %.2f s (first to last segment), %.0f B/s; "
               "PSH on %d; segment sizes %s" % (
                   len(data), total, t1 - t0, total / (t1 - t0) if t1 > t0 else 0,
                   sum("P" in p["flags"] for p in data),
                   ", ".join("%d x%d" % (s, c) for s, c in sorted(
                       {(p["len"], sum(q["len"] == p["len"] for q in data)) for p in data},
                       key=lambda x: -x[1])[:4])))
    delays, per_ack, turns, gaps = [], [], [], []
    unacked = []          # Mac segments not yet acknowledged: (end seq, arrival)
    acked = None
    last_ack_t = None     # a host ACK that acknowledged new data, not yet followed by Mac data
    prev_data_t = None
    retrans = 0
    seen = set()
    for p in pk:
        if p["frm"] == "mac" and p["len"] > 0:
            if (p["seq"], p["end"]) in seen:
                retrans += 1
            seen.add((p["seq"], p["end"]))
            unacked.append((p["end"], p["t"]))
            if last_ack_t is not None:
                turns.append(p["t"] - last_ack_t)
                last_ack_t = None
            if prev_data_t is not None:
                gaps.append(p["t"] - prev_data_t)
            prev_data_t = p["t"]
        elif p["frm"] == "host" and p["ack"] is not None and "S" not in p["flags"]:
            if acked is not None and p["ack"] <= acked:
                continue
            acked = p["ack"]
            cov = [u for u in unacked if u[0] <= acked]
            if cov:
                delays.append(p["t"] - max(u[1] for u in cov))
                per_ack.append(len(cov))
                unacked = [u for u in unacked if u[0] > acked]
                last_ack_t = p["t"]
    out.append(stats("ack delay", delays))
    out.append("  %-14s %s; ACKs held 30 ms or more: %d of %d; retransmitted segments: %d" % (
        "segs per ack", ", ".join("%d:%d" % (k, per_ack.count(k)) for k in sorted(set(per_ack))),
        sum(d >= 0.030 for d in delays), len(delays), retrans))
    out.append(stats("mac turn", turns))
    out.append(stats("seg gap", gaps))
    hi, holes = None, 0
    for p in data:
        if hi is not None and p["seq"] > hi:
            holes += 1
        hi = p["end"] if hi is None else max(hi, p["end"])
    out.append("  %-14s stalls of 0.5 s or more: %d; holes: %d" % (
        "lost", sum(g >= 0.5 for g in gaps), holes))
    return out


def recv(pk):
    data = [p for p in pk if p["frm"] == "host" and p["len"] > 0]
    if not data:
        return ["  no data from the host"]
    t0, t1 = data[0]["t"], data[-1]["t"]
    total = sum(p["len"] for p in data)
    seen, rex, delays, stalls = set(), 0, [], 0
    pend = []                      # host segments not yet ACKed by the Mac: (end seq, time)
    acked = None
    last = None
    for p in pk:
        if p["frm"] == "host" and p["len"] > 0:
            k = (p["seq"], p["end"])
            rex += k in seen
            seen.add(k)
            pend.append((p["end"], p["t"]))
            if last is not None and p["t"] - last >= 0.5:
                stalls += 1
            last = p["t"]
        elif p["frm"] == "mac" and p["ack"] is not None and "S" not in p["flags"]:
            if acked is not None and p["ack"] <= acked:
                continue
            acked = p["ack"]
            cov = [u for u in pend if u[0] <= acked]
            if cov:
                delays.append(p["t"] - min(u[1] for u in cov))
                pend = [u for u in pend if u[0] > acked]
    return ["  %d host data segments, %d bytes, %.2f s, %.0f B/s; retransmitted by the host: %d; "
            "stalls of 0.5 s or more: %d" % (len(data), total, t1 - t0,
                                             total / (t1 - t0) if t1 > t0 else 0, rex, stalls),
            stats("mac ack delay", delays)]


def echo(pk):
    host_echo, mac_turn, mac_ack = [], [], []
    last_mac, last_host, acked = None, None, True
    for p in pk:
        if p["frm"] == "mac":
            if p["len"] > 0:
                if last_host is not None:
                    mac_turn.append(p["t"] - last_host)
                    last_host = None
                last_mac = p["t"]
            elif last_host is not None and not acked and "F" not in p["flags"]:
                mac_ack.append(p["t"] - last_host)
                acked = True
        elif p["len"] > 0:
            if last_mac is not None:
                host_echo.append(p["t"] - last_mac)
                last_mac = None
            last_host, acked = p["t"], False
    return [stats("host echo", host_echo), stats("mac turn", mac_turn), stats("mac ack", mac_ack)]


def pauses(pk):
    """Mac data gaps over 0.5 s whose next segment the PC hadn't seen: the first copy was lost
    after the BlueSCSI took it (good WRITE status) and MacTCP re-sent it about 1 s later
    (hwsend10's frame log; hwsend9 read these as late ACKs). Returns (lost, gaps that ended in a
    segment the PC had seen)."""
    seen, last, n, nre = set(), None, 0, 0
    for p in pk:
        if p["frm"] != "mac" or not p["len"] or p["seq"] is None:
            continue
        if last is not None and p["t"] - last > 0.5:
            if p["seq"] in seen:
                nre += 1
            else:
                n += 1
        seen.add(p["seq"])
        last = p["t"]
    return n, nre


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("capture")
    ap.add_argument("--text", action="store_true", help="the argument is tcpdump -tt -n -S text")
    ap.add_argument("--mac", default="192.168.50.201", help="the Mac's address")
    ap.add_argument("--modes", default="EABCN", help="TcpSend's modes, in order (TcpSend.cfg)")
    a = ap.parse_args()
    if a.text:
        lines = open(a.capture).read().splitlines()
    else:
        r = subprocess.run(["tcpdump", "-r", a.capture, "-tt", "-n", "-S", "tcp"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("hwsend-acks.py: tcpdump -r failed: %s" % r.stderr.strip())
        lines = r.stdout.splitlines()
    order, conns = parse(lines, a.mac)
    sink_modes = [m for m in a.modes.upper() if m in "ABCN"]
    names = {"A": "A sync1400", "B": "B sync8k", "C": "C async4", "N": "N sync1400 nostamp"}
    k = 0
    print("hwsend capture summary: %s, Mac %s, %d TCP connection(s)" % (a.capture, a.mac, len(order)))
    tp = [pauses(conns[key]) for key in order if key[1] in (17031, 17007)]
    print("LOST: %d (Mac data gaps over 0.5 s ending in a segment the PC hadn't seen: a segment lost "
          "after a good WRITE and re-sent by MacTCP about 1 s later, hwsend10's frame log); gaps "
          "ending in a repeat the PC had seen: %d" % (sum(x for x, _ in tp), sum(y for _, y in tp)))
    for key in order:
        pk = conns[key]
        port = key[1]
        if port == 17031:
            label = names.get(sink_modes[k], "?") if k < len(sink_modes) else "extra"
            k += 1
        elif port == 17007:
            label = "E echo"
        elif port == 17030:
            label = "R recv"
        else:
            label = "other"
        print("\n== %s: Mac port %d -> %d, %d packets, start %.6f" % (label, key[0], port, len(pk),
                                                                     pk[0]["t"]))
        if port in (17031, 17007):
            n, nre = pauses(pk)
            print("  lost          %d after a good WRITE, %d repeats" % (n, nre))
        if port == 17031:
            print("\n".join(sink(pk)))
        elif port == 17007:
            print("\n".join(echo(pk)))
        elif port == 17030:
            print("\n".join(recv(pk)))
    if k and k != len(sink_modes):
        print("\nnote: %d sink connection(s) for modes %s: the labels may be off "
              "(N runs only with the diagnostic driver)" % (k, a.modes))


if __name__ == "__main__":
    main()

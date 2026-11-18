#!/usr/bin/env python3
"""test_hwsend_batch_term.py - scripts/hwsend-batch.py prints a section for MoofBatch's test T (TCP 17032).

hwsend-batch.py and hwsend-acks.py are hardware-session scripts that .gitignore keeps out of the repository
(`scripts/hwsend*`); without them, or without tcpdump, this exits 77 (ctest: Skipped). A synthetic raw-IP pcap holds
MARK datagrams and one T connection (3 rounds of a 1-byte `k` answered by 25 80-byte segments, then `q`); the
summariser's parser must find the section "== <setting> T#1" with the host segments and the PC's round time."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.normpath(os.path.join(HERE, "..", "scripts"))
BATCH = os.path.join(SCRIPTS, "hwsend-batch.py")
MAC, PC = "192.168.50.201", "192.168.50.190"


def ip(a):
    return bytes(int(x) for x in a.split("."))


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ipv4(proto, src, dst, payload):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 1, 0, 64, proto, 0, ip(src), ip(dst))
    h = h[:10] + struct.pack("!H", csum(h)) + h[12:]
    return h + payload


def tcp(src, dst, sp, dp, seq, ack, flags, data=b""):
    h = struct.pack("!HHIIBBHHH", sp, dp, seq, ack, 5 << 4, flags, 8192, 0, 0)
    return ipv4(6, src, dst, h + data)


def udp(src, dst, sp, dp, data):
    return ipv4(17, src, dst, struct.pack("!HHHH", sp, dp, 8 + len(data), 0) + data)


def pcap(path, pkts):
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 101))      # LINKTYPE_RAW
        for t, p in pkts:
            f.write(struct.pack("<IIII", int(t), int((t % 1) * 1e6), len(p), len(p)) + p)


class BatchTerm(unittest.TestCase):
    def test_t_section(self):
        t = 1000.0
        pk = [(t, udp(MAC, PC, 4000, 17040, b"MARK 7 new 1 T start"))]
        t += 0.01
        sp, dp = 5000, 17032
        ms, hs = 100, 5000                 # Mac and host sequence numbers
        pk += [(t, tcp(MAC, PC, sp, dp, ms - 1, 0, 0x02)), (t + 0.001, tcp(PC, MAC, dp, sp, hs - 1, ms, 0x12)),
               (t + 0.002, tcp(MAC, PC, sp, dp, ms, hs, 0x10))]
        t += 0.1
        for r in range(3):
            pk.append((t, tcp(MAC, PC, sp, dp, ms, hs, 0x18, b"k")))
            ms += 1
            for i in range(25):
                pk.append((t + 0.002 * (i + 1), tcp(PC, MAC, dp, sp, hs, ms, 0x18, b"x" * 80)))
                hs += 80
            t += 0.06                      # round: k to the last segment 50 ms
        pk.append((t, tcp(MAC, PC, sp, dp, ms, hs, 0x18, b"q")))
        pk.append((t + 0.01, udp(MAC, PC, 4000, 17040, b"MARK 7 new 1 T end")))
        d = tempfile.mkdtemp()
        try:
            f = os.path.join(d, "t.pcap")
            pcap(f, pk)
            r = subprocess.run([sys.executable, "-I", BATCH, f], capture_output=True, text=True)
        finally:
            shutil.rmtree(d)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = r.stdout
        self.assertIn("== new T#1", out)
        self.assertRegex(out, r"75 host data segments, 6000 bytes")
        self.assertRegex(out, r"term rounds\s+n\s+3 ")
        self.assertIn("lost          0 after a good WRITE", out)


if __name__ == "__main__":
    if not os.path.exists(BATCH) or not os.path.exists(os.path.join(SCRIPTS, "hwsend-acks.py")) \
            or shutil.which("tcpdump") is None:
        print("skipped: hwsend-batch.py / hwsend-acks.py (gitignored) or tcpdump missing")
        sys.exit(77)
    unittest.main()

#!/usr/bin/env python3
"""testnet.py - host-side TCP/UDP test services for MoofPort benchmarks.

Port of MoofTCP's scripts/testnet.py, kept to the services MoofPort's Phase 0 kit needs,
plus two bulk-throughput services:

  TCP 17007  echo     - echoes every byte back
  TCP 17009  discard  - reads and drops everything
  TCP 17023  banner   - sends a short banner on connect, then echoes (like MoofTCP's)
  TCP 17030  bulk source - on connect, sends BULK_BYTES bytes as fast as it can, then closes
  TCP 17031  bulk sink   - reads until the peer closes, then logs total bytes and bytes/s
  UDP 17007  echo     - echoes each datagram back to its sender
  UDP 17009  discard  - reads and drops each datagram
  UDP 17040  markers  - logs each datagram ("MARK <run> <setting> <n> <test> start|end", sent by
                        MoofBatch around each test) with the PC's time, so a run can be split

All sockets bind 0.0.0.0 (EMULATOR.md: Snow NAT forwards the guest to the host's own
interface IP, so the guest reaches these as 172.17.0.1:<port>). The bulk size is
BULK_BYTES (env, default 1048576). Log lines go to stdout; run it backgrounded with its
output redirected, as the run scripts do.
Echo and bulk lines end in "stalls N (...)" (gaps >= STALL_S); scripts/testnet-stalls.py splits
them per MoofBatch test.

Nothing here is Apple-copyrighted; it is an ordinary host test harness.
"""
import os
import socket
import sys
import threading
import time

HOST = "0.0.0.0"
ECHO_PORT = 17007
DISCARD_PORT = 17009
MARK_PORT = 17040
BANNER_PORT = 17023
BULK_SRC_PORT = 17030
BULK_SINK_PORT = 17031

BANNER = b"MoofPort test host\r\nlogin: "
BULK_BYTES = int(os.environ.get("BULK_BYTES", str(1024 * 1024)))
CHUNK = 4096


def log(msg):
    print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def serve_tcp(port, handler):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, port))
    s.listen(8)
    log("TCP %d listening" % port)
    while True:
        conn, addr = s.accept()
        t = threading.Thread(target=_wrap, args=(handler, conn, addr, port), daemon=True)
        t.start()


def _wrap(handler, conn, addr, port):
    log("TCP %d connect from %s:%d" % (port, addr[0], addr[1]))
    try:
        handler(conn, addr)
    except OSError as e:
        log("TCP %d %s:%d error %s" % (port, addr[0], addr[1], e))
    finally:
        try:
            conn.close()
        except OSError:
            pass
        log("TCP %d close %s:%d" % (port, addr[0], addr[1]))


# Stalls: a gap of STALL_S or more between two receives (or sends) on one connection. With the
# Mac's sync sends a lost segment shows up here as a ~1 s gap (MacTCP's retransmission), so the
# PC-side app can count them without a packet capture (hwsend auto1).
STALL_S = 0.5


class Stalls:
    def __init__(self):
        self.last = None
        self.gaps = []

    def tick(self):
        now = time.time()
        if self.last is not None and now - self.last >= STALL_S:
            self.gaps.append(now - self.last)
        self.last = now

    def text(self):
        return "stalls %d (%.2f s%s%s)" % (len(self.gaps), sum(self.gaps), ": " if self.gaps else "",
                                          " ".join("%.2f" % g for g in self.gaps))


def echo_handler(conn, addr):
    total = 0
    st = Stalls()
    while True:
        data = conn.recv(CHUNK)
        if not data:
            break
        st.tick()
        total += len(data)
        conn.sendall(data)
    log("TCP %d echoed %d bytes to %s; %s" % (ECHO_PORT, total, addr[0], st.text()))


def discard_handler(conn, addr):
    total = 0
    while True:
        data = conn.recv(CHUNK)
        if not data:
            break
        total += len(data)
    log("TCP %d discarded %d bytes from %s" % (DISCARD_PORT, total, addr[0]))


def banner_handler(conn, addr):
    conn.sendall(BANNER)
    total = 0
    while True:
        data = conn.recv(CHUNK)
        if not data:
            break
        total += len(data)
        conn.sendall(data)
    log("TCP %d banner+echo %d bytes to %s" % (BANNER_PORT, total, addr[0]))


def bulk_source_handler(conn, addr):
    """Send BULK_BYTES bytes as fast as possible, then close."""
    buf = bytes(bytearray((i & 0xFF) for i in range(CHUNK)))
    sent = 0
    st = Stalls()
    t0 = time.time()
    st.last = t0
    while sent < BULK_BYTES:
        n = min(CHUNK, BULK_BYTES - sent)
        conn.sendall(buf[:n])
        st.tick()
        sent += n
    dt = time.time() - t0
    rate = sent / dt if dt > 0 else 0.0
    log("TCP %d bulk source sent %d bytes in %.3fs (%.0f B/s) to %s; %s"
        % (BULK_SRC_PORT, sent, dt, rate, addr[0], st.text()))


# TcpBench's send pattern: a 1,400-byte chunk whose byte j is j & 255, repeated. The sink checks
# every byte against it, so a byte damaged on its way out of the Mac that still got through TCP
# (blind SCSI writes, after hw4) would show; a damaged segment that TCP's checksum caught shows
# in the kernel's InCsumErrors instead (scripts/csumwatch.py).
TCPBENCH_CHUNK = 1400
_PATTERN = bytes((j % TCPBENCH_CHUNK) & 0xFF for j in range(TCPBENCH_CHUNK * 50))


def bulk_sink_handler(conn, addr):
    """Read until the peer closes, then report bytes/s and pattern mismatches."""
    total = 0
    bad = 0
    first_bad = None
    t0 = None
    st = Stalls()
    while True:
        data = conn.recv(CHUNK)
        if not data:
            break
        st.tick()
        if t0 is None:
            t0 = time.time()
        off = total % TCPBENCH_CHUNK
        for i in range(0, len(data), TCPBENCH_CHUNK * 40):
            piece = data[i:i + TCPBENCH_CHUNK * 40]
            o = (off + i) % TCPBENCH_CHUNK
            want = _PATTERN[o:o + len(piece)]
            if piece != want:
                for k in range(len(piece)):
                    if piece[k] != want[k]:
                        bad += 1
                        if first_bad is None:
                            first_bad = total + i + k
        total += len(data)
    dt = (time.time() - t0) if t0 else 0.0
    rate = total / dt if dt > 0 else 0.0
    log("TCP %d bulk sink received %d bytes in %.3fs (%.0f B/s) from %s; TcpBench pattern: %s; %s"
        % (BULK_SINK_PORT, total, dt, rate, addr[0],
           "all bytes match" if bad == 0 else "%d bytes differ, first at offset %d" % (bad, first_bad),
           st.text()))


def serve_udp(port, drop):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, port))
    log("UDP %d listening (%s)" % (port, "discard" if drop else "echo"))
    while True:
        data, addr = s.recvfrom(65535)
        if not drop:
            s.sendto(data, addr)


def serve_marks(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, port))
    log("UDP %d listening (markers)" % port)
    while True:
        data, addr = s.recvfrom(512)
        log("UDP %d marker from %s: %s" % (port, addr[0], data.decode("ascii", "replace").strip()))


def main():
    global BULK_BYTES
    for arg in sys.argv[1:]:
        if arg.startswith("--bulk="):
            BULK_BYTES = int(arg.split("=", 1)[1])
        else:
            sys.stderr.write("testnet.py: unknown argument %r\n" % arg)
            return 2
    log("testnet.py starting (bulk size %d bytes)" % BULK_BYTES)
    threads = [
        threading.Thread(target=serve_tcp, args=(ECHO_PORT, echo_handler), daemon=True),
        threading.Thread(target=serve_tcp, args=(DISCARD_PORT, discard_handler), daemon=True),
        threading.Thread(target=serve_tcp, args=(BANNER_PORT, banner_handler), daemon=True),
        threading.Thread(target=serve_tcp, args=(BULK_SRC_PORT, bulk_source_handler), daemon=True),
        threading.Thread(target=serve_tcp, args=(BULK_SINK_PORT, bulk_sink_handler), daemon=True),
        threading.Thread(target=serve_udp, args=(ECHO_PORT, False), daemon=True),
        threading.Thread(target=serve_udp, args=(DISCARD_PORT, True), daemon=True),
        threading.Thread(target=serve_marks, args=(MARK_PORT,), daemon=True),
    ]
    for t in threads:
        t.start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        log("testnet.py stopping")
    return 0


if __name__ == "__main__":
    sys.exit(main())

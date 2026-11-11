#!/usr/bin/env python3
"""test_testnet_term.py - testnet.py's terminal service (TCP 17032, MoofBatch test T) on a free port.

A testnet.py --only-term runs on the port in TESTNET_TERM_PORT (a free one picked here), so a testnet already
running for a real Mac is never disturbed. A client sends `k` three times and must get exactly 2,000 bytes per
round (25 lines of 80 bytes) in at least 25 recv calls (one send per line, TCP_NODELAY, the client reads with
a small buffer); `q` closes; the log has one "term round" line per round."""
import os
import socket
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TESTNET = os.path.normpath(os.path.join(HERE, "..", "scripts", "testnet.py"))
ROUNDS = 3


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class TermService(unittest.TestCase):
    def setUp(self):
        self.port = free_port()
        env = dict(os.environ, TESTNET_TERM_PORT=str(self.port))
        self.proc = subprocess.Popen([sys.executable, "-I", TESTNET, "--only-term"], env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        else:
            self.fail("testnet.py --only-term did not listen on %d" % self.port)

    def tearDown(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.communicate(timeout=5)

    def test_rounds(self):
        c = socket.create_connection(("127.0.0.1", self.port), 5)
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for r in range(ROUNDS):
            c.sendall(b"k")
            got, calls = b"", 0
            while len(got) < 2000:
                d = c.recv(64)
                self.assertTrue(d, "connection closed in round %d after %d bytes" % (r, len(got)))
                got += d
                calls += 1
            self.assertEqual(len(got), 2000)
            self.assertGreaterEqual(calls, 25)
            lines = got.split(b"\n")
            self.assertEqual(len([x for x in lines if x]), 25)
            self.assertTrue(all(len(x) == 79 for x in lines if x))      # 79 + "\n" = 80 bytes
            c.settimeout(0.2)                                            # nothing more than 2,000 bytes
            with self.assertRaises(socket.timeout):
                c.recv(64)
            c.settimeout(5)
        c.sendall(b"q")
        self.assertEqual(c.recv(64), b"")                                # q closes
        c.close()
        time.sleep(0.2)
        self.proc.terminate()
        log = self.proc.communicate(timeout=5)[0]
        self.assertEqual(log.count("term round"), ROUNDS, log)
        self.assertIn("term: %d rounds" % ROUNDS, log)


if __name__ == "__main__":
    unittest.main()

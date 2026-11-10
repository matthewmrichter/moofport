# Phase 3: the fast path (in progress)

Driver 0.3 (`mac/driver/mp_drv.c`). What changed from Phase 1/2, and why:

| Change | Why (hw2, `BENCH.md` "hw2 findings") |
|---|---|
| Adaptive poll (core `mp_poll`): after RX or TX the next poll comes `burst` ms later (default 5), for `hold` ms after the last activity (60), then back off by doubling to `idle` (17, one tick, DaynaPORT's rate) | Phase 1/2 polled every 17 ms, so a reply waited up to a tick: echo RTT no better than DaynaPORT, bulk send 21% lower. Every empty READ costs about 3.5 ms of CPU on the Classic (hw1), so the burst is short and bounded and an idle Mac costs what it did |
| EWrite kick: an EWrite outside the poll task pulls the next poll forward to `burst` ms (Time Manager task re-armed with interrupts masked) | The ACK for a TCP send, or the AFP reply, arrives within a few ms; MacTCP's small send window stalls until it is read |
| Multi-packet READs (up to 4 per poll, each draining the device), immediate repoll when old firmware says more is queued, batched WRITE of deferred frames (new firmware) | already in the core; now also drive the poll rate |
| Blind payload reads (`SCSIRBlind`; the header and the 75/300 us pauses stay handshaked), **off by default** | PLAN 2.2 gate: a verifying probe must pass on hardware first |
| Verify mode: every received IPv4 frame's header and TCP/UDP checksums are checked (core `mp_ip_check`) and counted | the verifying probe: ordinary TCP traffic with blind reads on shows any byte that arrived wrong |
| Private Control `MP_CTL_TUNE` ('MT', `mp_drv.h`) and the MoofTune app (presets `default`, `phase1`, `verify`, `blind`, `fast`, or `MoofTune.cfg`) | poll-rate sweep and the blind gate on hardware without rebuilding; `phase1` gives the old behaviour for before/after numbers on the same boot |
| Stats v3 (tune, kicks, burst/immediate polls, verify counts), MoofStat prints them; the call log's first-24-calls ring dropped (336 bytes) | memory: Phase 3 is 17,304 fixed bytes (Snow, hw1c method), 750 over DaynaPORT, under the 17,578 cap (decision 4) |

Snow (functional only, no SCSI timing; NAT, MacTCP, MSS 536): TcpBench echo 64/64, receive
about 19-20 KB/s, send about 7 KB/s in every mode, as Phase 1. Verify mode: polled reads 590
frames checked, 0 bad; blind payload reads 589 frames, 0 bad.

hw3 on the Classic (`docs/dev/BENCH.md` "hw3 findings"): the blind gate passed (697 full-size
frames, 0 bad). The burst presets did not beat one poll per tick, and burst 3 ms cut TCP send
to 11.7 KB/s: each empty READ costs about 3.5 ms of CPU, so faster polling starves MacTCP.
Defaults since 0.3d2 (decision 5): one poll per tick, no burst, blind payload reads on.
MoofTune presets: `default` (these), `phase1` (polled reads), `burst` (0.3d1's 5 ms / 60 ms),
`verify`, `blind`, `fast`, `quiet` (one poll per 30 s, for ScsiProbe runs). Snow, 0.3d2 with no
tuning: TcpBench 64/64 echoes, MoofStat "BLIND payload", 0 bus errors. The next gain has to
come from a cheaper empty poll.

## Empty-poll probe (for hw4)

hw3 showed the empty READ (about 3.5 ms of CPU each) is what limits the Classic. ScsiProbe now
has two steps to find out where that time goes (`mac/scsiprobe/scsiprobe.c`):

- **Step 8 (on by default):** one empty READ through the SCSI Manager, timed call by call
  (SCSIGet, SCSISelect, SCSICmd, the 6-byte SCSIRead, SCSIComplete) and as one lap, 32 runs,
  min / median / max in microseconds. Timer: the revised Time Manager (prime with a negative
  count, RmvTime returns the time left); the cost of a lap is measured first and subtracted.
- **Step 9 (opt-in, cfg bit 9):** the same empty READ done directly on the 53C80 registers
  (arbitration as ID 7, selection, the CDB, then data in / status / message in as the target
  asks), split into those stages, with interrupts masked for the transaction. Register map from
  Snow's compact-Mac bus: base = low-memory SCSIBase (0x580000-0x5FFFFF; Snow uses 0x5FF000),
  register n read at base + 16n, written at base + 16n + 1.
- Before ScsiProbe, run MoofTune `quiet` (one MoofPort poll per 30 s) so the driver stays off
  the bus; a restart puts MoofPort back to its defaults. `ScsiProbe.cfg` = `770` runs steps 1,
  8 and 9.

Snow (functional check only, no SCSI timing): step 8 gave 32 empty READs and its split; step 9
read the empty 6-byte header, status 00 and message 00 on all 32 runs. On hardware the
comparison of step 8's "whole READ" with step 9's says how much of the 3.5 ms the SCSI Manager
adds; step 9's stages say how much is the bus and the BlueSCSI's response.

## After hw4: blind TX writes and idle back-off (0.3d3, both off by default)

- **Blind TX writes** (`MpTune.blind_tx`, MoofTune `txblind`): the frame bytes of each WRITE go
  with SCSIWBlind; the 4-byte preamble, the padding to 60 and the trailer stay handshaked. The
  core already marks those pieces (`mp_dev_tx.c`); the glue honours the hint only with the flag.
  Check on the PC: `scripts/csumwatch.py` logs the kernel's Tcp/Udp InCsumErrors and Ip
  InHdrErrors (a frame damaged before the BlueSCSI reaches the PC with a bad TCP checksum and is
  dropped there), and testnet.py's bulk sink now checks every byte against TcpBench's send
  pattern. AfpCopy uploads are checked with `scripts/afp-verify.py` on the server copy.
- **Idle back-off** (MoofTune `idle`): one poll per tick until 2 s after the last traffic, then
  one more tick per poll up to 3 ticks (51 ms). Any received frame snaps the next poll back to
  one tick, and an EWrite re-arms the timer at one tick at once (the existing kick). Worst case a
  frame arriving after a quiet spell waits 51 ms instead of 17 (an NBP lookup reply, an ARP
  reply); anything that follows comes at the normal rate. Saves up to two thirds of the empty
  polls (about 3.3 ms each) on an idle Mac.
- MoofTune `all` sets both. Stats v4 / tune v2 carry `blind_tx`; MoofStat prints it.
- Snow (functional): with `all`, TcpBench 64/64, 741 frames sent with blind writes, 0 errors;
  the sink got all 262,144 bytes; the idle poll rate dropped to about half of one per tick.

## 1.0b1 defaults (decision 6, 2026-11-08)

5 ms for 60 ms after traffic, one tick until 2 s of quiet, then out to three ticks (the core's
`mp_poll` middle level, `mid_ms`/`quiet_ms`, tested in `test_poll`); blind payload reads; polled
frame writes. Tune v3 / stats v5 carry the middle level. MoofTune presets: `default` (these),
`tick` (0.3d3), `burst`, `idle`, `txblind`, `all`, `phase1`, `verify`, `blind`, `fast`, `quiet`.
Snow, 1.0b1: TcpBench 64/64 with MacTCP (INIT install) and an EtherTalk boot (System-file
install), MoofStat shows the settings, 0 errors; no-device boot clean (`docs/dev/COMPAT.md`).

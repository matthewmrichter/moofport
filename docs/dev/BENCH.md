# Benchmarks: DaynaPORT 1.2.5 vs. MoofPort

Speed is quoted only from hardware columns (PLAN.md §4). Targets are from PLAN.md §5 and are
re-set as proper numbers after the hw1 baseline. Columns: **Snow-Dayna** (emulator,
DaynaPORT, for context only; Snow ticks are not Wi-Fi), **HW-Dayna** (hw1), **HW-MoofPort**
(hw2, later hw3). Blank = not measured yet. Procedure: `docs/dev/HARDWARE.md`. Raw session logs and the session
specs are kept outside the repository.

## Runs

| Run | Date | Firmware | Mac / MacTCP | Driver | Host and link | Checklist file |
|---|---|---|---|---|---|---|
| Snow-Dayna | 2026-10-31 | n/a | Mac Classic 4 MB / MacTCP 2.1 | DaynaPORT 1.2.5 | Snow v1.5.0, NAT to host | |
| hw1 | 2026-11-06 | 2026.08.17-rel Aug 17 2026 (Pico 2/2W; from `log.txt`, which was copied after the session) | Mac Classic 4 MB / MacTCP 2.1 | DaynaPORT 1.2.5 | Wi-Fi LAN to host <PC IP>; Dayna at SCSI ID 4 (image `NE4.hda`), boot disk ID 0 | (log kept outside the repo) |
| hw1b | 2026-11-06 | same as hw1 (not re-read) | Mac Classic 4 MB / MacTCP 2.1 | DaynaPORT 1.2.5 (SpyENET in the spy runs) | same LAN, host <PC IP>, Dayna at ID 4 | (log kept outside the repo) |
| hw1c | 2026-11-06 (evening) | same as hw1 (not re-read) | Mac Classic 4 MB / MacTCP 2.1, LocalTalk selected in Network, MultiFinder ON | DaynaPORT 1.2.5 | same | (log kept outside the repo) |
| hw2 | | | | MoofPort | | |
| hw3 | | | | MoofPort | | |

## Results

| Metric | Unit | Target (PLAN §5) | Snow-Dayna | HW-Dayna | HW-MoofPort | Pass |
|---|---|---|---|---|---|---|
| Added receive latency from polling | ms | <= 4 while active (Dayna up to 16.7) | | | | |
| Transmit latency (EWrite call to frame on the bus) | ms | < 2 when bus free (Dayna about 1 tick) | 14 ms (sync EWrite 0.85 ticks/call, EnetBench) | 18 ms (sync EWrite 1.10 ticks/call; async+wait 1.15 ticks) | | |
| MacTCP ping RTT to gateway, avg | ms | >= 15 lower than HW-Dayna | | see note 4 | | |
| EnetBench ICMP echo to router (extra) | ms | info | 26 ms (1.56 ticks) | 19 ms (mean 1.12 ticks, max 6 ticks; 64 replies, 0 lost) | | |
| TcpBench echo RTT (port 17007), avg / max | ms | lower than HW-Dayna | 521 / 533 ms (31.28 / 32 ticks), see note 1 | 34 / 83 ms (2.07 / 5 ticks; runs 2.10/5, 2.10/5, 2.01/3). hw1b tcp run: 35 / 66 ms (2.10 / 4 ticks); with spy 2.28 / 4 ticks |  40 / 150 ms (2.39 / 9 ticks; second run 2.25 / 5), hw2 | no |
| Keystroke echo feel in Telnet/Flynn | 1-5 | not worse than Dayna | | 3.5 | | |
| Bulk TCP receive through MacTCP | KB/s | >= 1.5 x HW-Dayna | 18.5 KB/s (18,929 B/s) | 45.5 KB/s (46,600 B/s; 1,345 / 1,354 / 1,362 ticks for 1,048,576 bytes, median 1,354). Host side 48,659 to 49,424 B/s. hw1b tcp run: 46,603 B/s (1,350 ticks, exactly 1,048,576 bytes: the count fix works), agrees with hw1; with SpyENET 43,873 B/s (1,434 ticks), spy costs about 6% |  56.3 KB/s (57,614 B/s; second run 55,627), hw2; Dayna same day 46,603 B/s: 1.24x | no |
| Bulk TCP send through MacTCP | KB/s | not lower than HW-Dayna | 6.6 KB/s (6,773 B/s) | 48.7 KB/s (49,900 B/s; 311 / 316 / 319 ticks for 262,144 bytes, median 316). Host sink 49,353 to 50,655 B/s. hw1b tcp run: 49,932 B/s (315 ticks); with spy 48,695 B/s (323 ticks) |  32.5 KB/s (33,323 B/s; second run 29,235), hw2; Dayna same day 42,395 B/s | no |
| Highest interrupt mask during driver work | level | not above poll task's level (Dayna: 6) | 6 (handler SR 0x2600) | 6 (handler SR 0x2600; OR of all entries 0x2604). hw1b spy: all 478 logged handler calls SR 0x2600 | | |
| CPU: idle loops/s (CpuMeter) | loops/s | no more than Dayna's cost | 48,814 | about 37,800 steady (mean 36,675 over 10 s, two slower seconds) | | |
| CPU: loops/s during bulk receive | loops/s | higher than Dayna | | | | |
| CPU: loops/s during bulk send | loops/s | higher than Dayna | | | | |
| Resident memory (see rule below) | bytes | <= HW-Dayna | Dayna 16,554 (Snow heap walk, same figure) | Dayna 16,554 fixed bytes (hw1c, LocalTalk boot, heap walk); see note 10 |  about 17,300 (+750 fixed bytes against Dayna, EtherTalk boot, hw2 note 2) | no (to confirm) |
| Raw: empty READ cost (EnetBench) | us | info | 0.10 ticks each = about 1,700 us (no timing model) | 0.21 ticks each = about 3,500 us (64 in 14 ticks; 0.20 in a second run), ScsiProbe | | |
| Raw: frames/s received / sent (EnetBench) | frames/s | info | 64 / 54 | 51 / 45 | | |
| Raw: polled vs blind 1514-byte read | us | info (blind go/no-go) | not meaningful in Snow (note 3) | polled: 39 frames in 25 ticks = about 10.7 ms per frame incl. command, about 128,000 B/s (128,100 and 126,000 B/s in two runs); blind (hw1b): header polled, payload blind, 5 frames / 6,136 bytes in 1 tick, err=0, against polled 43 frames / 58,004 bytes in 28 ticks (124,260 B/s). Provisional go, see note 9 | | |
| Raw: polled vs blind 1514-byte write | us | info (blind go/no-go) | | hw1b: polled 1 tick, blind 1 tick, both err=0 (1-tick resolution, no useful timing). Provisional go, see note 9 | | |
| Blind transfers bus-error at device pace | yes/no | decides Phase 3 split transfers | not meaningful in Snow | hw1b: no bus error or freeze in one blind write and one blind payload read. Provisional go, not proven (note 9) | | |
| TcpBench connect time | ms | info | 183 ms (fresh boot) | 116 / 133 / 183 ms (median 133). hw1b: tcp run 1,566 ms (94 ticks, first network use after a restart, outlier); spy run 216 ms (13 ticks) | | |

Snow-Dayna notes:

1. Echo RTT is set by Snow's NAT delayed ACK (`EMULATOR.md`, 100-210 ms), not by the driver. Raw
   ICMP echo through the same driver is 26 ms (1.56 ticks).
2. During-bulk CPU rows are blank: CpuMeter cannot run during a transfer without MultiFinder, and on
   hardware, with MultiFinder on, a concurrent run did not work either (hw1 step 11).
3. Polled vs blind read: Snow sends up to 8 packets per READ whatever the flag says (PLAN 1.3).
   Probe 7 takes the first and `SCSIComplete` drains the other seven, about 89 ticks. That gives
   exactly 2 frames per round of 16: 1 + 7, 1 + 7, then empty. Real firmware sends one packet
   with CDB[5] = 0x80, so the probe is right for hardware and its Snow rate (960 vs 1,020 B/s)
   measures that drain, not the bus. The write row is blank: probe 4 only checks that one blind
   WRITE completes. Probe 6 (real disk contention): the task ran 1,232 times during a 1,086,981-byte
   File Manager read: 73 got the bus, 1,159 scMgrBusyErr, 0 other.

HW-Dayna notes (hw1, 2026-11-06; ticks converted at 60.15 per s; KB = 1024 bytes):

4. Ping: the ping tool used (name not recorded) gave min/avg/max 0/9/14 ticks, out 192, in 20,
   bad 172 (89% "bad"). Out = in + bad, so every ping was answered and the tool rejected most
   replies. What "bad" means depends on the tool. OPEN. No value is entered. The EnetBench ICMP
   row above is the usable gateway figure.
5. Resident memory: the hw1 readings do not measure Dayna's footprint; see the README caveats
   in the raw session logs (kept outside the repository). Memory was answered by hw1c: 16,554 bytes (note 10).
6. Bulk receive corrected for the +8,192 count (note below): all rates here use 1,048,576 bytes.
   The tool printed 47,142 / 46,828 / 46,553 B/s from the inflated count.

7. hw1b (2026-11-06): raw files kept outside the repository. TcpBench prints B/s using 60 ticks
   per second; at 60.15 per s the tcp receive is 46,720 B/s. hw1 rows above use 60.15, so
   compare like with like (the difference is 0.25%).

### hw7 findings (2026-11-08): release candidate 1.0b1 (before the day of use)

Raw logs kept outside the repository. MoofPort 1.0b1, the 1.0 defaults (decision 6). Tools in hw2
conditions. 1 tick = 16.63 ms.

**A. Memory** (LocalTalk boot, HeapInfoOpen, the hw1c method): opening MoofPort costs **17,530
fixed bytes** of system heap (application heap +0, zone +0). DaynaPORT: 16,554. So +976 bytes,
**48 bytes under** decision 4's cap (17,578). Within the rule, but with almost no room: any
growth of the driver for 1.0 needs a matching trim (the budget model said 17,520).

**B. Raw driver rate** (EnetBench, MacTCP closed, same router, against hw1's DaynaPORT)

| | DaynaPORT 1.2.5 (hw1) | MoofPort 1.0b1 | |
|---|---|---|---|
| synchronous EWrite | 1.10 ticks/call (18 ms) | **0.55 ticks/call (9 ms)** | 2x faster |
| asynchronous EWrite + wait | 1.15 ticks/call | 0.75 ticks/call | 1.5x |
| ARP requests sent / replies received | 45 req/s / 51 frames/s | **65 req/s / 66 frames/s** | 1.44x / 1.29x |
| ICMP echo to the router, mean (max) | 1.12 ticks = 19 ms (6) | 0.84 ticks = 14 ms (8) | 25% lower |

Handlers were called with SR 0x2104 (interrupt mask 1, the Time Manager task's level), where
DaynaPORT used mask 6 (PLAN 2.5 / CLAUDE.md: not copied on purpose; MacTCP and AppleTalk work).

**So the TCP send gap is MacTCP-side, not the driver's.** At the raw level MoofPort sends twice as
fast per call and answers echoes faster than DaynaPORT; it is only through MacTCP's TCP that
sending ends up near or below DaynaPORT. The likely cause is how MacTCP paces its send window
against the timing of incoming ACKs; it is not something a faster driver path fixes (blind
writes and the burst did not move it, hw5 and hw6).

**C. With MacTCP and AppleShare**

| Measure | hw2 DaynaPORT | hw6 0.3d3 (default / burst) | hw7 1.0b1, t1 / t2 |
|---|---|---|---|
| TCP receive | 46,603 | 73-77k / 79-85k | **85,714 / 88,737** (1.84x / 1.90x) |
| TCP send | 42,395 | 34-35k | 30,481 / 38,740 (0.72x / 0.91x) |
| Echo mean (max) | 33 ms (50) | 42-47 ms | 41 ms (100) / 42 ms (183) |
| AfpCopy to server | 21,260 | 14-86k (noisy) | **88,600 / 88,982** (4.2x) |
| AfpCopy from server | 18,366 | 34-41k | 36,537 / 44,016 (2.0x / 2.4x) |

(B/s.) Host checks: both TcpBench sends "all bytes match", kernel checksum errors 0, both
AfpCopy runs byte-exact, server copy identical. MoofStat over the session: 4,512 frames in, 3,618
out, 0 bus errors, 0 overflow, 0 aborted or bad READs, 0 failed sends.

1.0b1 is the best release yet on receive and AppleShare. Send varies 0.72-0.91x DaynaPORT;
echo is 8-9 ms slower. Nothing in hw7 blocks 1.0; the day of use is the remaining check.

### hw6 findings (2026-11-07): idle back-off and burst, clean re-test (0.3d3)

Raw logs kept outside the repository. Tools in hw2 conditions. 1 tick = 16.63 ms.

**A. Idle CPU** (just after a restart, AppleShare not mounted, nothing else open; MoofStat
before and after each 10 s CpuMeter run)

| Setting | Polls in the window | Frames received in it | CpuMeter mean (per-second range) |
|---|---|---|---|
| defaults (one poll per tick) | 2,254 | 65 | 34,092 loops/s (33,606-34,404) |
| `idle` (back off to 3 ticks after 2 s quiet) | 1,432 (-36%) | 29 | 35,668 loops/s (33,969-39,451) |

With `idle`, the seconds with no traffic ran at 37,900-39,500 loops/s (11-16% more CPU than the
defaults; hw1's DaynaPORT idle was about 37,800), and the seconds after a received frame
(broadcasts: ARP, AppleTalk) fell back to the default rate for the 2 s hold. Over the window: 36%
fewer polls and 4.6% more CPU. It works as designed; how much it gives depends on how chatty the
network is. hw5's opposite result (less CPU with `all`) was not reproduced. The Chooser time
was not noted; hw5's first echo after 20 s of quiet was 37 ms mean, 67 ms max.

**B. Default against burst** (5 ms for 60 ms after traffic), interleaved d1, b1, d2, b2

| Measure | default d1 / d2 | burst b1 / b2 | Difference |
|---|---|---|---|
| TcpBench send | 35,108 / 33,897 | 34,342 / 35,504 | none (means 34.5 vs 34.9 KB/s) |
| Echo RTT mean (max) | 47 (150) / 42 (100) ms | 44 (100) / 44 (100) ms | none |
| TcpBench receive | 73,241 / 76,631 | 78,939 / 84,904 | burst +9% (both burst runs above both default runs); 84.9 KB/s = 1.82x DaynaPORT, the best yet |
| AfpCopy to server | 13,599 / 72,434 | 86,375 / 67,907 | noise: 13.6 to 86.4 KB/s (d1 waited on 1,109 empty READs) |
| AfpCopy from server | 34,463 / 35,592 | 40,717 / 35,048 | small or none |

Host checks over the session: every TcpBench send "all bytes match", kernel checksum errors 0,
every AfpCopy byte-exact, last server copy identical. MoofStat-type counters in every AfpCopy: 0
errors, 0 failed sends.

**What this says about the send gap.** TCP send is 34-35 KB/s in every hw4-hw6 configuration
(polled or blind writes, one tick or a 5 ms burst), against DaynaPORT's 42 KB/s. Neither faster
ACK pickup (burst) nor faster payload transfer (blind writes) moved it, and echo is unchanged by
the burst too. So the gap is not the poll rate and not the frame transfer. What is left on the
driver side is the fixed cost of each WRITE (selection about 1.2 ms + command + preamble +
status, one WRITE per frame). Whether that or something MacTCP-side (how it paces its window
against our timing) explains the remaining 20% is not settled by these runs; a raw EWrite rate
test (EnetBench, as in hw1, where DaynaPORT managed 45 frames/s) would separate the two.

### hw5 findings (2026-11-07): blind TX writes and idle back-off (0.3d3)

Raw logs kept outside the repository. Tools in hw2 conditions (no yielding). 1 tick = 16.63 ms.

| Run | Writes | Poll | Echo mean (max) | TCP receive | TCP send | AfpCopy to / from server |
|---|---|---|---|---|---|---|
| hw2 DaynaPORT | | one tick | 33 ms (50) | 46,603 | 42,395 | 21,260 / 18,366 |
| hw4 0.3d2, two runs | polled | one tick | 38 / 43 ms | 71,331 / 70,295 | 32,699 / 28,339 | 42,652 / 34,349; 71,185 / 32,820 |
| hw5 `t0`/`a0` (defaults) | polled | one tick | 45 ms (266) | 71,412 | 33,394 | 78,493 / 37,878 |
| hw5 `t1`/`a1` (`txblind`) | blind | one tick | 40 ms (183) | 76,445 | 36,663 | 18,921 / 38,300 |
| hw5 `t2`/`a2` (`txblind`) | blind | one tick | 45 ms (200) | 77,385 | 21,487 | 42,829 / 35,840 |
| hw5 `i1` (`all`, after 20 s quiet) | blind | idle back-off | 37 ms (67) | 78,643 | 34,952 | 39,623 / 31,907 |

(B/s; the host's own bulk-sink figures agree within 2%: 32,844 / 36,736 / 21,439 / 35,075.)

**Blind TX writes: safe.** Every TcpBench send with blind writes (t1, t2, i1: 786,432 bytes,
about 540 full frames) reached the PC with **all bytes matching** TcpBench's pattern; the kernel's
Tcp/Udp InCsumErrors and Ip InHdrErrors stayed at **0** for the whole session; every AfpCopy
(about 800 frames up each) came back byte-exact, and the last server copy is identical
(`afp-verify.py`). MoofStat: 5,212 frames sent, 0 failed, 0 errors, 0 bus errors.

**Blind TX writes: no measurable gain.** TCP send 36.7 and 21.5 KB/s with blind writes against
33.4 polled; uploads 18.9 and 42.8 KB/s against 78.5 polled (hw4 polled: 42.7 and 71.2). The
run-to-run spread (2-4x on uploads) is far larger than any effect. The payload transfer is not
what limits sending.

**Idle back-off: inconclusive.** CpuMeter idle: 33,914 loops/s with the defaults (`c0`), 30,705
with `all` (`c1`), i.e. 9% *less* CPU left with back-off, which the design cannot cause (the
back-off never polls more often than the defaults), so something else was busy during `c1`
(AppleShare mounted in both). `MoofStat-all`, which would show the poll count, was not returned,
nor the Chooser observation. First echoes after 20 s of quiet were fine (`i1`: 37 ms mean, max
67 ms, connect 50 ms). For reference, hw1 measured about 37,800 loops/s idle with DaynaPORT.

Against PLAN section 5: receive is now 1.53-1.69x DaynaPORT (target **met**, best yet); send
0.51-0.86x (**missed**); echo 37-45 ms vs 33 (**missed**); AppleShare about 2x. No driver
errors in any run.

Note on hw3's conclusion: the burst experiments ("faster polling does not help") were measured
with the yielding TcpBench, which hid even the blind-read gain. Send and echo, the two misses,
are exactly what a burst after each send should help (the ACK is read sooner), so hw3 did not
settle that question.

### hw4 findings (2026-11-07): MoofPort 0.3d2, and where an empty poll's time goes

Raw logs kept outside the repository. MoofPort 0.3d2 (decision 5: one poll per tick, blind payload reads),
EtherTalk + MacTCP. TcpBench and AfpCopy ran **without** yielding, as in hw2, so section A
compares directly with hw2. 1 tick = 16.63 ms.

**A. 0.3d2 against hw2 (two runs each, to show the noise)**

| Measure | hw2 DaynaPORT | hw2 MoofPort 0.2 | hw4 0.3d2 run 1 / run 2 | 0.3d2 vs DaynaPORT | PLAN section 5 target | Met |
|---|---|---|---|---|---|---|
| TcpBench echo RTT mean (max) | 33 ms (50) | 40 ms (150) | 38 ms (166) / 43 ms (150) | 1.13-1.29x slower | lower than DaynaPORT; ping 15 ms lower | no |
| TcpBench receive | 46,603 B/s | 57,614 B/s | 71,331 / 70,295 B/s | **1.53x / 1.51x** | >= 1.5x | **yes** |
| TcpBench send | 42,395 B/s | 33,323 B/s | 32,699 / 28,339 B/s | 0.77x / 0.67x | not lower | no |
| AfpCopy to server | 21,260 B/s | 54,183 B/s | 42,652 / 71,185 B/s | 2.0x / 3.3x | (none) | |
| AfpCopy from server | 18,366 B/s | 40,007 B/s | 34,349 / 32,820 B/s | 1.9x / 1.8x | (none) | |

MoofStat after A (`MoofStat-a.out`): 132,308 polls, 3,218 busy (2%), 128,356 of 136,306 READs
empty (94%), 8,190 frames received (234 READs brought more than one), 3,988 sent (1 deferred);
0 bus errors, 0 overflow, 0 aborted or bad READs, 0 failed EWrites.

**B. Both at once** (run id `both`, yielding): AfpCopy PASS at 27,859 / 24,576 B/s; TcpBench
64/64 echoes but one echo waited 2,903 ticks (48 s) and receive fell to 12,006 B/s, send 33,182
B/s. TcpBench is in the background while AfpCopy (and its run-id dialog, and the Finder) are in
front, and only gets the time they leave it; the long echo is that wait, not lost frames (no
driver errors).

**C. One empty READ, split** (`ScsiProbe-p.out`; 32 runs; min / median / max microseconds, the
cost of a timing lap subtracted). Step 9, the raw 53C80 path, ran on the Classic for the first
time: no freeze; 31 runs recorded, 1 failed at selection (stage -4, the target did not answer in
time), 27 of 31 empty.

| Stage | SCSI Manager (step 8) | Raw 53C80 (step 9) |
|---|---|---|
| Get / arbitration | 163 / 224 / 1,021 | 306 / 306 / 326 |
| Select / selection | 1,266 / **1,307** / 2,124 | 1,164 / **1,184** / 1,205 |
| Command, 6 bytes | 388 / 429 / 1,327 | 2,430 / 2,634 / 2,655 |
| 6-byte header (SCSIRead) | 204 / 265 / 469 | (with the next row) |
| Complete / status + message | 326 / 347 / 32,353 | 2,430 / 2,634 / 66,401 |
| **Whole empty READ, one lap** | 2,859 / **3,308** / 35,355 | 8,840 / 9,003 / 72,341 |

Notes:

1. **Receive meets the 1.5x target; send and echo do not.** Against hw2's 0.2, receive is
   +22-24%: the blind payload reads (hw3's yielding TcpBench hid this). Send is 0.67-0.77x
   DaynaPORT and echo 13-29% slower. AfpCopy is about 2x DaynaPORT both ways; the two runs to
   the server differ by 67% (42.7 vs 71.2 KB/s), so single AfpCopy runs are rough.
2. **The biggest single cost of an empty poll is the BlueSCSI's answer to selection: about 1.2-1.3
   ms of the 3.3 ms**, the same on both paths. That is the device's latency (the firmware noticing
   SEL and asserting BSY), not Mac software. Arbitration is about 0.2-0.3 ms. The rest, about 1.3
   ms through the SCSI Manager (CDB, 6-byte header, status and message, i.e. 14 byte handshakes,
   plus the SCSI Manager's own work), is Mac-side and protocol work together.
3. **My raw 53C80 path is about 3x slower than the SCSI Manager** (9.0 vs 3.3 ms): its byte
   handshakes take about 330-440 us each against 45-70 us through the SCSI Manager. The handshake
   loop is unoptimised C with a call per register access and a long REQ wait loop; Apple's is
   tight assembly. So the probe shows the raw path works on the Classic, but not yet that it can
   be faster; the selection wait, the largest part, would stay.
4. Echo maxima and the 32 ms / 35 ms outliers in step 8 are interrupts (the Mac's tick, the
   Finder) landing inside a lap; the minimum and median are the clean figures.

### hw3 findings (2026-11-07): MoofPort 0.3 (Phase 3), presets on one boot

Raw logs kept outside the repository. MoofPort 0.3d1 as the System-file `.ENET0`, EtherTalk + MacTCP. Steps
C to E on one boot, F after a restart. 1 tick = 16.63 ms. **The hw3 TcpBench and AfpCopy yield
under MultiFinder (they did not in hw2), so hw3 numbers compare with each other, not with hw2's
(note 2).**

| Run (hw3 unless noted) | Poll: burst / hold / idle ms | Echo RTT mean / max | TCP receive | TCP send | AfpCopy to / from server |
|---|---|---|---|---|---|
| hw2 DaynaPORT (sync tools) | (one tick) | 33 / 50 ms | 46,603 B/s | 42,395 B/s | 21,260 / 18,366 B/s |
| hw2 MoofPort 0.2 (sync tools) | 17 / - / 17 | 40 / 150 ms | 57,614 B/s | 33,323 B/s | 54,183 / 40,007 B/s |
| `phase1` (C) | 17 / 0 / 17 | 78 / 1,364 ms | 45,689 B/s | 32,974 B/s | 44,683 / 25,869 B/s |
| `default` (C) | 5 / 60 / 17 | 75 / 1,447 ms | 40,563 B/s | 29,676 B/s | 34,637 / 25,517 B/s |
| `verify` (E, polled, checksums on) | 5 / 60 / 17 | 96 / 1,646 ms | 47,446 B/s | 23,903 B/s | - |
| `blind` (E, blind payload, checksums on) | 5 / 60 / 17 | 79 / 1,696 ms | 46,672 B/s | 30,422 B/s | 30,314 / 24,459 B/s |
| `fast` (F) | 3 / 100 / 17 | 82 / 1,879 ms | 50,778 B/s | **11,737 B/s** | - |
| both at once (D, `default`): TcpBench / AfpCopy | 5 / 60 / 17 | 94 / 2,428 ms | 7,007 B/s | 11,997 B/s | 37,062 / 30,903 B/s |

Against PLAN section 5:

| Target | Result | Met |
|---|---|---|
| AppleShare works with MoofPort (Phase 2 row), also with MacTCP traffic at once | every AfpCopy PASS, byte-exact, including D; server copy identical | yes |
| Echo / ping RTT at least 15 ms lower than DaynaPORT | not lower; the burst presets do not lower it either | no |
| Bulk receive >= 1.5x DaynaPORT | at most 1.09x (fast) of hw2 DaynaPORT; no preset clearly beats `phase1` | no |
| Bulk send not lower than DaynaPORT | lower in every run; `fast` 3.6x lower | no |
| Blind-read gate (PLAN 2.2): a few hundred full frames, bytes compared | 697 frames of 1,000+ bytes (1,605 IPv4 frames) checked with blind reads, 0 bad; 0 bus errors; no freeze | **yes** |
| Resident memory <= DaynaPORT + 1,024 (decision 4) | 17,304 (Snow, hw1c method); see note 5 | yes |

Notes:

1. **Faster polling does not help on the Classic, and very fast polling hurts.** `default`
   (burst 5 ms) is no better than `phase1` (one poll per tick) on any measure, and `fast`
   (burst 3 ms) cuts TCP send to 11.7 KB/s. Each empty READ costs about 3.5 ms of CPU (hw1),
   and the poll runs as a Time Manager task: at a 3 ms burst the poll takes almost all of the
   CPU while it lasts, and MacTCP, which sends from task level, starves. In AfpCopy-d1 to the
   server 492 of 699 READs were empty (70%), about 1.7 s of CPU in a 9.9 s copy. The hw2 theory
   (send waits for ACKs that are read late) was wrong as the main cause: what limits the
   Classic is CPU, and the empty poll is the most expensive thing the driver does. The next gain
   has to come from a cheaper empty poll, not a more frequent one.
2. **Tools changed between hw2 and hw3.** TcpBench now issues MacTCP calls asynchronously and
   calls WaitNextEvent while it waits, and AfpCopy yields every 16 KB, so the Finder (whose open
   window is updating with AfpCopy's files) gets time in between. Echo maxima went from 3-9
   ticks to 82-146 ticks in every run including `phase1`, and AfpCopy from the server from 40 to
   about 25 KB/s with the same driver timing. TcpBench and AfpCopy need a switch: yield only for
   the side-by-side step, run as in hw2 for measurements.
3. **Single runs, sizeable noise.** Three runs with the same `default` timing (d1, v, b) gave
   receive 40.6 / 47.4 / 46.7 KB/s and send 29.7 / 23.9 / 30.4 KB/s (verify adds a checksum
   pass per frame). Differences under about 15% are not significant here.
4. **Both at once works.** TcpBench and AfpCopy ran side by side, AppleTalk and IP through one
   poll loop: AfpCopy PASS at 37 / 31 KB/s, TcpBench 64/64 echoes; TcpBench's throughput drops
   to 7-12 KB/s because AfpCopy's synchronous File Manager calls hold the CPU between its
   yields. MoofStat over the session: 0 bus errors, 0 overflow, 0 aborted or bad READs, 0
   failed EWrites.
5. **Memory:** `HeapInfo-mem` shows 499,490 fixed bytes, 11,500 below hw2's boots with either
   driver, with 17-19 fewer blocks: something else in the system heap differs between boots
   (AppleShare, the Chooser), so whole-heap totals across boots cannot compare drivers to the
   byte. The Snow hw1c-method figure (17,304) stands.
6. **Blind reads pass the gate but showed no speed gain** (receive 46.7 vs 47.4 KB/s polled,
   both with verify on). The payload transfer is not where the time goes; per-poll overhead is.

### hw2 findings (2026-11-07): MoofPort on the real Classic

Raw logs kept outside the repository. MoofPort 0.2d1 as DRVR 47 in the System file (MoofPortInstall),
EtherTalk and MacTCP at the same time, against DaynaPORT 1.2.5 on the same Mac and the same day.
One run each unless noted. 1 tick = 16.63 ms (60.15/s).

| Measure | DaynaPORT | MoofPort | Ratio | PLAN section 5 target | Met |
|---|---|---|---|---|---|
| AppleShare (AfpCopy, 336 KB, byte-exact both ways) | PASS | PASS | | AppleShare row passes on hardware | yes |
| AfpCopy to server | 21,260 B/s (16.18 s) | 54,183 B/s (6.35 s) | 2.55x | (no target) | |
| AfpCopy from server | 18,366 B/s (18.73 s) | 40,007 B/s (8.60 s) | 2.18x | (no target) | |
| TcpBench echo RTT, mean / max | 33.4 / 50 ms (2.01 / 3 ticks) | 39.7 / 150 ms (2.39 / 9 ticks); second run 37.4 / 83 ms (2.25 / 5) | 1.19x slower | lower than Dayna; ping >= 15 ms lower | no |
| TcpBench bulk receive | 46,603 B/s | 57,614 B/s; second run 55,627 B/s | 1.24x | >= 1.5x (about 68 KB/s) | no (better, short of 1.5x) |
| TcpBench bulk send | 42,395 B/s | 33,323 B/s; second run 29,235 B/s | 0.79x | not lower than Dayna | no |
| System heap fixed bytes, first after restart | 510,990 | 511,740 | +750 | MoofPort <= Dayna | no (see note 2) |
| Bus errors / RX overflow / aborted or bad READs / failed EWrites | | 0 / 0 / 0 / 0 | | none | yes |

MoofStat after the AppleShare steps (`MoofStat-afp.out`): 24,105 polls, 2,209 skipped because
the bus or SCSI Manager was busy (9%), 23,837 READs of which 21,504 empty (90%), 2,476 frames
received (140 READs returned more than one), 2,410 frames sent (1 deferred, 0 failed). Handlers:
AppleTalk type 0 (802.3) 1,198 frames, IP 1,190, ARP 72; 16 frames for no handler.

Notes:

1. **AppleShare is the clear win.** File copies through AppleShare over EtherTalk are 2.2 to
   2.6 times faster with MoofPort, both ways, with every byte checked. Everything was reported
   working as expected.
2. **Memory: +750 bytes of fixed system heap against DaynaPORT, one boot each.** By the hw1c
   method that puts MoofPort at about 17,300 bytes against the 16,554 cap. The driver code is
   7,852 bytes (working target 6,000) plus 3 RX + 2 TX slots (7,620). The zone also grew
   (588,728 to 591,396) and MaxMem after purging was 9 KB higher with MoofPort, so this needs a
   block-level breakdown before any conclusion: one boot each, and AppleTalk may size its own
   buffers from what the driver reports. **Follow-up in Snow (2026-11-07):** opening the
   System-file MoofPort after a LocalTalk boot (HeapInfoOpen, the hw1c method) costs 16,388
   fixed bytes, 166 under DaynaPORT's 16,554: DRVR 7,852 + state block 8,460 (rings 7,620, call
   log 496, rest 344) + block headers and the DCE. So the driver's own blocks are under the cap;
   the hw2 difference comes from elsewhere at an EtherTalk boot (not broken down further). Same day, it was decided that about 1 KB over DaynaPORT is acceptable if trimming would cost performance (PLAN
   decision 4); cap now 17,578.
3. **Latency and send are not better yet, as expected before Phase 3.** MoofPort polls once per
   17 ms Time Manager tick, the same period as DaynaPORT, so the echo RTT floor is the same
   (2 ticks); the mean is a little higher. Bulk send is 21% lower: each EWrite is one immediate
   single-frame WRITE, and MacTCP's send window waits for ACKs that are only picked up at the
   next 17 ms poll. Receive is 24% higher from the multi-packet READ and zero-copy delivery.
   Both point at Phase 3's adaptive polling (burst after traffic) and TX batching.
4. **Test tool: TcpBench does not yield under MultiFinder.** It runs its whole benchmark without
   calling WaitNextEvent, so the Mac cannot switch to another application until it ends. The
   "both at once" step needs TcpBench (or AfpCopy) to yield between phases and in its waits.
   Not a driver problem: TcpBench ran normally and its numbers are in the table.
   **Fixed (2026-11-07):** TcpBench issues every MacTCP call asynchronously and calls
   WaitNextEvent (sleep 0) while it waits; AfpCopy yields after each 16 KB chunk; both have a
   SIZE resource with canBackground and a 384 KB partition (`mac/common/bgsize.r`). In Snow the
   Finder comes forward while TcpBench runs and AfpCopy starts beside it; TcpBench's own numbers
   are unchanged (echo 29.6 ticks, receive 18,212 B/s, send 6,947 B/s, Snow NAT).
5. Not measured in hw2: added receive latency from polling, transmit latency, interrupt mask,
   CPU cost (no CpuMeter/EnetBench in the kit), ping. Those rows stay open for Phase 4.

### hw1c finding (2026-11-07)

10. **Memory measured: 16,554 bytes.** Raw logs kept outside the repository. LocalTalk boot, heap walk. Dayna
   1.2.5 costs 16,554 fixed bytes in the system heap on the real Classic (purgeable +0, zone +0,
   application heap +0). This matches Snow's heap walk, and replaces the Snow FreeMem figure 15,466.

### hw1b findings (2026-11-06)

8. **Memory still not measured.** `HeapInfoOpen-A3`: `OpenDriver(.ENET0)` = 0, refnum -48, open
   cost fixed +0, purgeable +0, app +0. The driver was already open before the tool ran
   (AppleTalk/EtherTalk opens `.ENET0` at boot, `docs/dev/BOOT-CRASH-ETHERTALK.md`). So Dayna's
   footprint was NOT measured in hw1b. Answered by hw1c (note 10); Snow's heap walk (opened
   through SpyENET) had already given 16,554 fixed.
9. **Blind transfers: provisional go.** Blind 1514-byte WRITE: err=0, no freeze, no bus error.
   Header-polled, payload-blind READ: err=0, no freeze, but only 5 frames / 6,136 bytes in 1 tick
   (about 1,227 bytes per frame, so not all were full replies) against polled 43 frames / 58,004
   bytes in 28 ticks. The probe does not verify the bytes, the sample is small, 1 tick is the
   timer's resolution, and the 368,160 B/s the tool prints is not a rate. So: no bus error or
   hang at the device's pace on the real Classic in this run. Data integrity is unverified.
   A bigger blind run that compares bytes is needed before Phase 3 relies on it.

| System heap (bytes) | Fixed | Purgeable | Free | Zone |
|---|---|---|---|---|
| A1 (first read after restart) | 497,898 | 9,882 | 81,308 | 589,140 |
| A2 | 497,898 | 20,808 | 70,382 | 589,140 |
| A3 before and after HeapInfoOpen | 497,898 | 20,808 | 70,382 | 589,140 |
| A4, A5 | 497,898 | 20,808 | 70,382 | 589,140 |

Fixed is stable. Between A1 and A2 purgeable grew 10,926 and free fell 10,926 (most likely
resources the first run loaded that stay cached as purgeable; fixed, the footprint measure, did
not change). Restart reads A6 to A8 were not sent.

### hw1 ScsiProbe findings

- SCSI ID 4 (Snow uses 3).
- Multi-packet READ (CDB[5] 0xC0) returns a first header with flags 0x10 ("more follows"). This
  firmware supports multi-packet READ.
- err=5 (scPhaseErr) in 5A/5B/5C is the tool asking for 3,000 bytes while the device ends
  sooner. Expected, not a failure.
- 5B (two SCSIRead calls per command, the DV14 case) gave e1=0: the first call works.
- Probe 6: during a 1.1 MB File Manager read (1,103,365 bytes), 969 of 1,093 SCSIGet attempts
  from a Time Manager task got scMgrBusyErr and none failed otherwise (124 got the bus). The busy
  check works on real hardware.
- The cfg-150 run (blind probes) did not read its cfg; see the 2026-11-06 note.

Pass column: yes / no / n/a once HW-MoofPort is filled.

## Method notes

- **Common conditions.** Same Mac, same SD card, same Wi-Fi, router and LAN host, MacTCP 2.1
  with the same address settings. Restart before each group. Run each timed test three times
  and enter the median, noting the spread in Notes. Firmware version goes in the Runs table.
- **Added receive latency from polling.** Not directly visible from the Mac. Derived from
  EnetBench frame timing and the echo RTT: HW-Dayna tick-limited figure vs. HW-MoofPort. Until
  EnetBench defines a per-frame figure, use echo RTT difference as a proxy and say so.
  OPEN: define once EnetBench's output is final.
- **Transmit latency.** EnetBench: time from the EWrite call to the write command completing
  on the SCSI bus, bus free. Snow numbers are in ticks.
- **MacTCP ping RTT.** A MacTCP ping tool sending 20 pings to the router, average of the
  replies. If no ping tool is available, TcpBench echo RTT to the LAN host stands in, and the
  row is labelled "echo" for all columns (do not mix).
- **Echo RTT.** TcpBench against port 17007: send a small payload, wait for it back, repeat 50
  times; report average and maximum.
- **Keystroke feel.** Subjective, a tester's 1 to 5 in Flynn/Telnet on port 17023. Used only as
  a sanity check.
- **Bulk receive / send.** TcpBench against 17030 (host sends) and 17031 (Mac sends). KB/s is
  1024 bytes per second over a fixed size (the tool states it), from connect-complete to last
  byte. MacTCP caps these (MSS 536, window about 4 KB). The published reference is 60 to 80
  KB/s on a faster Mac (PLAN.md §1.1), so a Classic should be lower.
- **CPU left over.** CpuMeter busy-loop counter. Idle = nothing else running. During bulk =
  the same counter read while TcpBench transfers. Higher loops/s is better. Report loops/s
  and as a percentage of the idle value.
- **Interrupt mask.** From SpyENET's logged SR (Dayna: 0x2600 in every handler call, PLAN.md
  §1.2). For MoofPort, from the driver's own log ring or MacsBug.
- **EnetBench rows.** Run with MacTCP closed. Values come from the tool's result block.
  Blind transfers: go/no-go is "go" only if no bus error or hang over a full run.
- **Memory.** From the HeapInfo family (HARDWARE.md "Memory check"). The comparison rule:
  take system-heap occupied bytes (zone size minus FreeMem), second run on a fresh boot. MacTCP
  stays installed but unopened, because without its sysz the system heap has about 14 KB free
  and OpenDriver(.ENET0) fails with -1 (measured: zone 162,100, FreeMem 13,930). Dayna footprint
  = occupied after opening `.ENET0` minus occupied before. MoofPort footprint = boot-time delta
  of its INIT (boot with vs. without the INIT) plus its open delta. MoofPort passes if its
  footprint is less than or equal to Dayna's. Fill both footprints in the row as
  `Dayna X / MoofPort Y` and record the underlying figures below. Snow reference: opening
  MacTCP's `.IPP` costs 15,952 bytes in Snow (whole stack, includes Dayna's open).
- **The 1,078 bytes.** HeapInfo reads 1,078 lower than the open's own figure in two cases: once
  after the control panel (91,818, then 92,896), and on every read after HeapInfoOpen (91,964).
  The 1,078-byte block IS part of Dayna's footprint (corrected 2026-11-07): Snow's FreeMem figure
  15,466 was 1,088 bytes low against the heap-walk figure 16,554, and hw1c measured 16,554 on the
  real Classic. The block that appeared on the next read after an open is Dayna's. The cap is
  16,554. (The earlier wording here, that the block "cannot be Dayna's memory", was wrong.)
- **MacTCP's first network use** adds about 4,522 (92,896 to 88,374), matching MoofTCP's figure.

### Memory figures (bytes, system heap)

| State | Zone size | FreeMem | Occupied | Source run |
|---|---|---|---|---|
| MacTCP installed, not opened | 435,772 | 108,508 | 327,264 | Snow 2026-10-31 |
| MacTCP installed, `.ENET0` opened (HeapInfoOpen, at return) | 435,772 | 93,042 | 342,730 | Snow 2026-10-31 |
| MacTCP active idle (after control panel, second read) | 435,772 | 92,896 | 342,876 | Snow 2026-10-31 |
| After TcpBench | 435,772 | 88,374 | 347,398 | Snow 2026-10-31 |
| MoofPort INIT, MacTCP installed, not opened (hw2) | | | | |
| MoofPort INIT, MacTCP installed, `.ENET0` opened (hw2) | | | | |
| hw1c LocalTalk boot, system before open (HeapInfoOpen) | 575,672 | 64,086 | fixed 490,726 | hw1c 2026-11-06 |
| hw1c LocalTalk boot, system after open (HeapInfoOpen) | 575,672 | 47,532 | fixed 507,280 | hw1c 2026-11-06 |
| Dayna footprint (fixed, system heap; purgeable +0, app heap +0) | | | 16,554 | hw1c (real Classic, heap walk); Snow heap walk also 16,554. Snow FreeMem figure 15,466 was low by 1,088 |
| MoofPort footprint (Snow, Phase 1, 3 RX + 2 TX) | 435,772 | 93,214 | +15,294 fixed at boot, +0 at open = 15,294 | Snow 2026-11-07 (HeapInfo-base / -mp, heap walk) |

## Notes

Dated notes, one per surprise, with the row they affect and the run that showed it.

- **2026-10-31, Memory.** Opening `.ENET0` with MacTCP parked (no sysz) fails with -1: zone
  162,100, FreeMem 13,930. So MacTCP stays installed, unopened, for the baseline.
- **2026-10-31, Bulk receive.** Every Snow run ended with -23005 (connectionClosing) after
  148,912 of 1,048,576 bytes, the same number each time. The rate is still over the bytes
  received. Cause suspected but NOT proven: Snow's NAT dropping data when the host closes at
  once. hw1 must check the total received.
- **2026-10-31, Memory budget.** The plan budget of 16,144 (PLAN 2.4) was 678 bytes over the
  Snow FreeMem cap 15,466. Superseded 2026-11-07: the measured cap is 16,554, so 4 RX + 2 TX
  (16,144) fits with 410 to spare and 3 RX + 2 TX (14,620) with 1,934. See the hw1c note below.
- **2026-11-07, hw1c memory (Memory row, Dayna footprint).** Real Mac Classic, LocalTalk
  selected in the Network control panel (nothing opened `.ENET0` at boot), MultiFinder ON,
  hw1b heap-walk HeapInfo. `OpenDriver(.ENET0)` = 0, refnum -48. System heap open cost: fixed
  +16,554, purgeable +0, zone +0; application heap +0. Before fixed 490,726, after 507,280,
  zone 575,672. Plain HeapInfo 2nd (before) 490,726; 3rd and 4th (after) 507,274: difference
  16,548, agreeing with the direct figure. 1st run: fixed 489,726, purgeable 9,882 (first run
  loads cached resources, as in hw1b). Dayna 1.2.5's resident footprint is 16,554 fixed bytes
  of system heap: THE MEMORY CAP. It equals Snow's heap-walk figure; Snow's FreeMem figure
  15,466 was 1,088 low and the 1,078-byte block was part of Dayna's footprint. Raw files in
  (raw logs kept outside the repository).
- **2026-10-31, MAC address.** The guest MAC changes on every Snow boot.
- **2026-10-31, ScsiProbe (steps 254).** All probes run without a hang in Snow. An earlier build
  hung with all probes on; probe 6 was then rewritten and the old build was never bisected, so
  its `SCSIComplete` from a task at mask 1 (Ticks cannot advance there) is the likely cause, not a
  proven one. Probe 7's Snow rate is not a measurement (note 3).
- **2026-11-06, Bulk receive count (TcpBench).** Every hw1 run shows 1,056,768 bytes received,
  8,192 too many. On the closing call the tool added the buffer length. The host log shows exactly
  1,048,576 bytes sent each time, so the transfer was complete. HW-Dayna rates use 1,048,576.
  Fix in hw1b.
- **2026-11-06, Snow bulk receive truncation.** The 148,912-byte stop in Snow (note of
  2026-10-31) is a Snow artifact: hardware received all 1,048,576 bytes. The Snow rate stays
  context only.
- **2026-11-06, SpyENET.** No capture: SpyDump said ".ENET0 is not the spy". Likely the real
  driver was already open as ".ENET0" before the spy's INIT ran (AppleTalk at boot), or the INIT
  found no free unit-table entry. Fix in hw1b. The hardware contract is still the Snow one.
- **2026-11-06, ScsiProbe cfg 150.** The run did not read its cfg: it shows the default steps
  0x8e and the Snow addresses. The blind write/read probes never ran on hardware. Blind go/no-go
  is OPEN; the rows say "not run (cfg not read)".
- **2026-11-06, Ping "bad" count.** out 192 = in 20 + bad 172. All pings were answered; the tool
  rejected 89%. Meaning depends on the tool. OPEN. The BlueSCSI log ends at the Wi-Fi connect line
  and holds no runtime network lines, so it cannot explain this.
- **2026-11-06, Firmware.** `log.txt` reads 2026.08.17-rel Aug 17 2026, Pico 2/2W, Dayna at ID 4
  from image `NE4.hda`, boot disk at ID 0 (1.1 GB). The file was copied after the session and
  ends at the Wi-Fi connect line; it has no SCSI, error, queue or drop lines, so it says nothing
  about multi-packet use. The redacted log copy is kept outside the repository.
- **2026-11-06 (hw1b), Memory.** HeapInfoOpen opened an already-open driver (cost 0). Dayna's
  footprint is still not measured; see note 8. The question whether EtherTalk was selected is
  answered by this: `.ENET0` was open at boot. MultiFinder was on (2026-11-07).
- **2026-11-06 (hw1b), Spy.** With AppleTalk holding `.ENET0` the spy worked (hw1's did not
  capture). 2,289 events, 1,266 dropped (ring 1,024). Calls: EAttachPH 2, EWrite 1,173, EGetInfo
  1, ESetGeneral 1, other 0; 1,111 handler deliveries. All EWrites async, 0 immediate, all under
  mask, 1 inside a handler, max 3 WDS pieces, frame 42 to 1,454. Same call order as Snow.
- **2026-11-06 (hw1b), Spy poll pattern.** In the 478 logged handler deliveries (474 IP, 4 ARP)
  SR was 0x2600 every time. D1 values: 1504 x324, 452 x81, 50 x72, 92 x1. D1 1504 is a
  1,500-byte IP datagram, so MacTCP's MSS on the real LAN is 1,460 (Snow behind smoltcp: 536).
  D1 50 is a frame padded to 60 by the firmware (Snow pads to 64, D1 54). Never more than one
  delivery in the same tick (478 deliveries in 478 distinct ticks). Gaps between delivery ticks:
  1 tick x124, 2 x332, 3 x16, 4 x3, 5 x1, 6 x1. Over 859 ticks that is 33.5 deliveries/s during
  bulk receive. EWrite sizes logged (540 of the 1,173): 54 x472 (ACKs), 55 x64, 42 x2, 58 x2.
  Max over the whole run 1,454 (1,400-byte TcpBench sends + 54).
- **2026-11-06 (hw1b), Missing from the return.** Not sent: A6 to A8 (restart plus control
  panel reads), the `MoofPort Run` file, and the Network control panel state (MultiFinder was on, 2026-11-07).
- **2026-11-06, Memory.** The readings do not measure Dayna's footprint (purging MaxMem, system
  heap growth 591,360 to 621,352, and EtherTalk may have opened `.ENET0` at boot). OPEN: was
  EtherTalk or LocalTalk selected, and was MultiFinder on? Rerun with the fixed HeapInfo in hw1b.

## What hw1 means for the open design choices

Facts, not decisions. PLAN.md decisions are not changed.

- **Dayna is faster than PLAN 1.2 estimated, but not by delivering more than one frame per
  tick.** Bulk receive is 45.5 KB/s, not the "near 32 KB/s" estimate. The earlier reading of
  this (hw1: "Dayna delivers more than one frame per tick, MSS 536 at 46,600 B/s is about 87
  segments/s") was wrong. The real LAN MSS is 1,460, so 46,600 B/s is about 32 full frames/s,
  and the hw1b spy shows at most one delivery per tick. Receive under Dayna is bounded by one
  frame per tick, in practice about one per two ticks with full frames, so about 32 frames/s.
  The "1.5x Dayna" target (about 68 KB/s) means roughly 47 full frames/s, i.e. more than one
  frame per tick. That needs draining several frames per poll (multi-packet READ is supported
  by the firmware, hw1 probe 5). Worth re-checking the target after hw2.
- **Polling cost.** One empty READ costs about 3.5 ms on the bus. A burst poll every 2 to 4 ms
  (PLAN 2.2) would spend most of the time on empty READs. The burst interval and the idle back-off
  must be set with this number. The drain-until-empty rule also costs one empty READ (3.5 ms) per
  poll.
- **Polled full-frame transfer** is about 10.7 ms per frame including the command (about 128,000
  B/s). That bounds receive at about 90 full frames/s on the bus before any other cost. This is the
  figure blind transfers would have to beat. Blind got a provisional go in hw1b (note 9); the
  bytes were not checked, and a blind read yielded only 5 frames in its tick.
- **Multi-packet READ** is supported by the firmware (flag 0x10 seen), so draining several frames
  per command is possible. How much it saves per frame is not measured yet.
- **TX batching** is not measured on hardware. Sync EWrite costs 18 ms per frame under Dayna, so
  per-frame write overhead is large. Batching's value is still OPEN.
- **RX slot count (3 vs 4) and the memory cap** cannot be decided from hw1 or hw1b, because
  Dayna's footprint was not measured in hw1 or hw1b (driver already open at boot); hw1c measured it: 16,554 bytes, which is the cap.
- **Busy-bus behaviour.** SCSIGet from a Time Manager task is reliably refused during disk I/O on
  real hardware (969 of 1,093). The poll task's back-off design relies on this.
- **EtherTalk.** If EtherTalk is in use, `.ENET0` is opened at INIT time, so MoofPort must
  survive that (already a requirement in `docs/dev/BOOT-CRASH-ETHERTALK.md`).
- **Memory (hw1c).** The cap is 16,554 bytes, measured on the real Classic. The old default
  4 RX + 2 TX (16,144) fits with 410 bytes to spare; the approved default 3 RX + 2 TX (14,620)
  leaves 1,934. The default stays 3 + 2 (approved 2026-11-07); whether to go to 4 + 2 is
  OPEN, a project decision.

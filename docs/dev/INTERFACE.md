# The observed MacTCP 2.1 to .ENET0 contract

What MacTCP 2.1 actually does to an `.ENET0` driver, as seen by SpyENET. Companion to
`PLAN.md` section 1.4 (the documented API) and MoofTCP's `MACTCP-INTERFACE.md`.

**Source.** SpyENET (`mac/spyenet/`) in Snow v1.5.0, Mac Classic 4 MB, System 6.0.8, MacTCP 2.1,
DaynaPORT SCSI/Link 1.2.5 behind the spy. Load: TcpBench (echo plus bulk) against
`scripts/testnet.py`. Raw log: `SpyENET.txt` in the Phase 0 scratchpad
(`runs/e/SpyENET.txt`), not committed. Marks: **observed** = seen in that log; **OPEN** = not
seen, to settle on hardware or by design.

Scope: only these runs. No MacTCP shutdown, control-panel change or link-down was captured.

## 1. Finding the driver

| Fact | Status |
|---|---|
| `OpenDriver(".ENET0")` reaches the driver by name. The spy sat in a unit-table entry above MacTCP's (unit 52 or higher, not refnum -48) and MacTCP's open reached it. So a unit-table driver named `.ENET0` wins over `DRVR 47`, and MacTCP does not need refnum -48. | observed |
| The control panel still showed "Ethernet Built-In" selected with the spy in place. | observed |
| How MacTCP builds that list (which resources or unit entries it scans) | OPEN |

## 2. Call sequence at start

All sync, SR 0x2008 (mask 0).

| Order | Call | Detail | Status |
|---|---|---|---|
| 1 | Open | trap 0xA000, mask 0 | observed |
| 2 (about 2 s later) | EGetInfo (252) | eBuffSize 6 | observed |
| 3 | ESetGeneral (253) | eBuffSize 6, result 0 | observed |
| 4 | EAttachPH 0x0800 | IP | observed |
| 5 | EAttachPH 0x0806 | ARP | observed |

- ESetGeneral IS called. This contradicts PLAN 1.4's "not needed". The driver must accept it
  and return 0. What the call is meant to do (the 6-byte buffer content, for example a MAC
  to set) is OPEN.
- EGetInfo returns only the 6-byte MAC (DaynaPORT behaviour). Nothing else it reports was seen
  to matter. **observed**

## 3. Never called in this run

EAddMulti, EDelMulti, EDetachPH, ERead, ERdCancel, Close, Prime, Status: zero calls (the
`count:` line shows "other csCodes 0"). This covers only these runs. EDetachPH and Close are
**OPEN**: no MacTCP shutdown, control-panel change or link-down was captured. The driver must
still implement all of them.

## 4. EWrite

| Fact | Value | Status |
|---|---|---|
| Calls | 742 | observed |
| Async (trap 0xA404) | 742 of 742 | observed |
| Immediate / sync | 0 / 0 | observed |
| ARP frames | ioCompletion 0 | observed |
| IP frames | completion routine set | observed |
| WDS piece 1 | always 14 bytes (Ethernet header) | observed |
| WDS piece 2 | 28 (ARP), 44 (SYN), 40 (IP+TCP headers) | observed |
| WDS piece 3 | data, when present | observed |
| Max pieces | 3 | observed |
| Frame length | min 42, max 590 | observed |
| Padding by MacTCP | none: ARP goes out at 42, pure ACKs at 54 | observed |

The driver must pad frames to 60 bytes (before CRC). The first piece holds only the header, so
the driver gathers pieces; it must not assume one contiguous buffer.

Frame max 590 means MSS 536 (536 + 40 + 14). Whether MTU or MSS depends on anything the driver
reports: **OPEN**, beyond noting that EGetInfo returns only the MAC.

## 5. Protocol handlers

| Fact | Value | Status |
|---|---|---|
| Handlers attached | exactly two, IP (0x0800) and ARP (0x0806); none for other types | observed |
| Logged deliveries | 511 (510 IP, 1 ARP) | observed |
| SR in the handler | 0x2600 (mask 6) on all 511, under DaynaPORT | observed |
| D1 values | 54 x246, 580 x256, 513 x6, 111, 188, 503 x1 each | observed |

580 = 536 + 40 + 4. D1 counts 4 bytes past the IP datagram, which confirms the rule that D1 =
frame length - 14 + 4 (the 4 CRC bytes). 54 = a 64-byte frame - 14 + 4: Snow pads short frames
to 64 (CLAUDE.md device facts); the log records D1 only, so the frame length is inferred.

The SR 0x2600 is DaynaPORT's behaviour. What MacTCP needs at the handler is not known to depend
on it. Handlers may be entered from any mask the driver chooses: OPEN beyond mask 6 working.

## 6. Contexts of EWrite

| Fact | Value | Status |
|---|---|---|
| EWrites seen at mask 1 or higher | 742 of 742 ("under mask 742") | observed |
| SR of logged EWrites | 0x2108 x283, 0x2608 x224 (507 logged) | observed |
| EWrite seen inside a handler | 1 (depth 1: the SYN after the ARP reply) | observed (lower bound) |

**Caveat.** The spy serialises calls on its own Device Manager queue. An EWrite issued from
inside a handler can be dispatched later from the previous write's IODone. Logged depth and SR
describe when the spy ran the call, not always when MacTCP issued it. So "inside handler: 1" is
a lower bound.

**Design rule.** EWrite must work async at any mask and from inside a handler. Never block, never
wait on the device in that path.

## 7. Log coverage

| Fact | Value |
|---|---|
| Ring | 1023 entries logged (1024 slots) |
| Events | 1340 total, 317 dropped |
| `count:` lines | cover every event |
| Entries | do not cover the end of the run (the ring filled; later events are counted only) |

## 8. Spy overhead

Negligible: bulk receive 18,929 vs 18,849 B/s (without vs with), bulk send 6,773 vs 6,642 B/s.
So the spy does not distort the timing it observes. **observed**

## 9. Still OPEN

- Behaviour at MacTCP shutdown and control-panel changes (EDetachPH, Close).
- What ESetGeneral does.
- Whether MTU/MSS depends on anything the driver reports.
- Whether MacTCP ever calls EAddMulti/EDelMulti (UDP multicast, IGMP), ERead, Status, Prime.
- EWrite issued truly inside a handler, without spy serialisation (hardware, MoofPort's own log).

## 10. Hardware (hw1b, 2026-11-06)

Source: SpyENET on the real Mac Classic, DaynaPORT 1.2.5 behind the spy, AppleTalk holding
`.ENET0`. Raw SpyENET log kept outside the repository (1,023 of 2,289 events logged).
Sections 1 to 9 above stay as the Snow record.

| Fact | Value | Status |
|---|---|---|
| Call sequence | open (sync, SR 0x2008), EGetInfo (eBuffSize 6), ESetGeneral (eBuffSize 6, result 0), EAttachPH 0x0800, EAttachPH 0x0806. Same csCodes and order as Snow | observed |
| Other csCodes | 0 calls | observed |
| EWrite | 1,173 calls, all async, 0 immediate, all under mask, 1 inside a handler, max 3 WDS pieces | observed |
| EWrite frame length | min 42, max 1,454 here (1,400-byte TcpBench sends + 54). Logged sizes: 54 x472 (ACKs), 55 x64, 42 x2, 58 x2 | observed |
| MSS on a real LAN | 1,460, so frames reach 1,514 both ways (Snow behind smoltcp: 536, frame 590) | inferred from RX D1 1,504; TX max 1,454 only reflects the 1,400-byte test sends |
| RX handler D1 | 1504 x324, 452 x81, 50 x72, 92 x1 (478 logged) | observed |
| Short frames | the firmware pads to 60 bytes, so D1 is 50 (Snow pads to 64, D1 54) | observed |
| Handler SR | 0x2600 (mask 6) on all 478 logged deliveries | observed |
| Delivery rate | at most one handler delivery per tick (478 in 478 distinct ticks). Gaps: 1 tick x124, 2 x332, 3 x16, 4 x3, 5 x1, 6 x1. 33.5 deliveries/s over 859 ticks of bulk receive | observed |
| `.ENET0` opened at boot | AppleTalk/EtherTalk opens `.ENET0` before MacTCP. The spy then sees MacTCP's own open as the first call to it, and the first call is not the system's first open | observed (spy worked); see `docs/dev/BOOT-CRASH-ETHERTALK.md` |

Design consequences: the driver must be safe to be opened at INIT time by AppleTalk; buffers
must hold 1,514-byte frames; do not assume more than one delivery per tick under Dayna, but
beating Dayna needs several frames per poll. Still OPEN on hardware: EDetachPH, Close,
EAddMulti, ERead, Status, Prime, and what ESetGeneral does.

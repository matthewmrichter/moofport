# DaynaPORT's send and reply path, traced in Snow (2026-11-08)

Why does DaynaPORT SCSI/Link 1.2.5 turn a TCP send round in about 26 ms, where MoofPort's send-fix takes about 38-42 (hwsend5, mode A)? This traces its behaviour only, with no disassembly.

## Method

- SpyENET (`mac/spyenet/`, the Phase 0 pass-through `.ENET0` shim) sits between MacTCP 2.1 and DaynaPORT's driver. It logs every EWrite and every handler delivery with TickCount and SR.
- For this trace it also head-patches `_SCSIDispatch` (A815) when it opens the real driver. It logs every `SCSICmd` sent to the DaynaPORT (SCSI ID 3 in Snow's workspace, tracked through the last `SCSISelect`) as a `scsi` line with the CDB, in the same ring.
- Logging starts at the first EWrite of 500 bytes or more, the bulk send. Snow's NAT gives 590-byte segments.
- The run: fresh golden image with DaynaPORT, SpyENET installed, SpyDump to empty the ring, TcpSend mode A (sync 1400 sends to the bulk sink), SpyDump. The ring held the first 1,017 events: 536 ticks, 44 segment rounds.
- Snow does not model SCSI or Wi-Fi timing, so the tick counts below show order and cadence, not hardware speed.

## Findings

| | DaynaPORT (this trace) | MoofPort send-fix |
|---|---|---|
| READ CDB | `08 00 00 05 F4 C0`: multi-packet flag, allocation 1,524. The firmware returns at most one full frame. | `08 .. .. 06 86 C0` for 3 free slots (allocation 1,670): also one full frame, or up to 3 small ones |
| WRITE CDB | `0A 00 00 LL LL 80`: preamble WRITE, one frame | the same form; batched WRITEs on new firmware |
| Where the WRITE happens | **inline, inside MacTCP's EWrite**. The WRITE's SCSICmd follows the EWrite directly, at SR 0x2609 (mask 6). | EWrite copies into the TX ring; the poll task writes it about 1 ms later |
| Poll cadence | about 1.3 READs per tick: 350 ticks with 1 READ, 122 with 2, 33 with 3, 6 with 4. Always at mask 6 (SR 0x2609), above the VBL and Time Manager level. | burst 5 ms after traffic, then 17, then 51; at mask 1 (a Time Manager task) |
| READ after a WRITE | **right away**: 83 of 126 WRITEs are followed by a READ in the same tick, 42 in the next | not sooner than 5 ms (send-fix default since hwsend6) |
| Handler call | SR 0x2600, from inside its poll | SR 0x2104, from inside our poll tick |
| ACK handed to MacTCP → MacTCP's next EWrite | **the same tick in 24 of 44 rounds, the next tick in 20** | 15-19 ms (hwsend2 / hwsend5 diagnostic) |

## What it means

1. **DaynaPORT's wire and reply path is not where it wins.** Its READ is our READ, and its WRITE is our WRITE. Its WRITE is just done at once, in MacTCP's call, instead of a millisecond later from a queue.
2. **DaynaPORT reads right after a WRITE, often in the same tick, and lost no frames on hardware (hwsend5).** So "a READ 2 ms after a WRITE loses frames", the explanation offered for hwsend5's stalls with the 2 ms poll, can't be the whole story. What differs is unknown. Candidates: our blind payload reads, our 4-frame READ allocation, our chase issuing several READs in a row.
3. **The difference is how soon MacTCP sends after the ACK.**
   - With DaynaPORT, MacTCP's next EWrite comes in the same or the next tick.
   - With us it comes 15-19 ms after the handler returns.
   - MacTCP sends from its own Time Manager task B, primed "as soon as possible" from its handler (`MACTCP-SEND-TRIGGER.md`, branch `mactcp-trigger-probe`). That task can't run until the interrupt it was primed from has returned.
   - DaynaPORT's poll runs at mask 6, so when it returns, the Time Manager interrupt (level 1) is serviced at once and B runs.
   - Ours runs inside the Time Manager interrupt itself, at level 1. B is queued behind our own task and runs after it. So **our whole tick after the delivery delays MacTCP**: the extra READ (fixed in hwsend6 by yielding after a small frame), the chase's re-arming, the WRITE of any queued frame.
4. **A cheaper single-frame READ isn't supported by the data.** DaynaPORT's READ is the same multi-packet form, and with our allocation the firmware already returns one full frame. Not added.

## Next proposals (not done; each needs approval)

- **Write in MacTCP's EWrite when the TX ring is empty (as DaynaPORT does).** This saves the copy and the wait for the next tick, about 2.5 ms per segment. With blind writes a full-frame WRITE is about 6.4 ms, under one 60 Hz period. Keep the queue for bursts, so two back-to-back WRITEs never run inside one interrupt-level call and the ticks aren't lost again.
- **Measure hwsend6's yield on hardware first.** If handler → next EWrite drops from 15-19 ms toward DaynaPORT's same-tick behaviour, the rest of the gap is the write path above.

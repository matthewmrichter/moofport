# MoofPort: project plan

MoofPort is a greenfield Ethernet driver for the BlueSCSI v2 Wi-Fi DaynaPORT emulation, used in
place of the DaynaPORT SCSI/Link driver (installed by the "DaynaPORT 7.5.3" package) on a
Macintosh Classic running System 6.0.8 with MacTCP.

- **Goal:** maximum network throughput and the lowest possible latency for MacTCP on a
  68000 Mac, within the limits of the hardware.
- **Hard constraints:** fully MacTCP compatible on System 6.0.8, and resident memory no more
  than about 1 KB over the DaynaPORT driver it replaces (decision 4).
- **Status:** Phase 6, release candidate 1.0b1 on hardware (hw7, 2026-11-08): receive 1.9x DaynaPORT, AppleShare upload 4.2x / download 2-2.4x, send 0.72-0.91x (MacTCP-side: raw EWrite is 2x DaynaPORT's), memory 17,530 bytes (48 under the cap). Waiting for the day-of-use result. Decision 6 applied; `docs/dev/COMPAT.md`, `scripts/release.sh`.

This is the "separate, later project" that MoofTCP's spec held in reserve: "if it becomes
evident ... that [DaynaPORT's] SCSI polling sets the latency floor, ... a replacement driver
would be a separate, later project" (`../mooftcp/docs/superpowers/specs/2026-10-28-mooftcp-design.md`
§3). MoofPort sits *under* the TCP stack, so it speeds up MacTCP 2.1 today and MoofTCP later,
with no change to either.

---

## 1. What we know already (evidence)

Every fact below has a source. **[fw]** is the BlueSCSI v2 firmware source
(`github.com/BlueSCSI/BlueSCSI-v2`, `lib/SCSI2SD/src/firmware/network.c`, read at commit
`5e64e3b`, 2026-10-03). **[snow]** is Snow's emulation (`twvd/snow`,
`core/src/mac/scsi/ethernet.rs`). **[mt]** is MoofTCP's measurements in Snow
(`../mooftcp/docs/dev/MACTCP-INTERFACE.md` "Ethernet driver calls", `EMULATOR.md`,
`GOLDEN-IMAGE.md`). **[hw1]** is hardware session 1 on the real Classic with DaynaPORT
(`docs/dev/BENCH.md`; raw session logs are kept outside the repository). **[hw1b]** is hardware session 1b,
SpyENET and a verifying probe on the same Classic (`docs/dev/BENCH.md`, `docs/dev/INTERFACE.md`
section 10; raw session logs are kept outside the repository). **[bs]** is https://bluescsi.com/docs/WiFi-DaynaPORT.

### 1.1 The setup being replaced

| Item | Value | Source |
|---|---|---|
| Driver | `DRVR 47 ".ENET0"` inside the System file, DaynaPORT **SCSI/Link 1.2.5** (the 7.5.3 package selects it). Code resource is 12,512 bytes | [mt] |
| Install | Dayna Installer, Custom Install, "DaynaPORT SCSI/Link" only. No AppleTalk, no EtherTalk, no Network cdev, no ADEV | [mt], [bs] |
| TCP/IP | MacTCP "2.1" (BlueSCSI-distributed, a patched 2.0.6). Apple's 2.0.6 shows only LocalTalk with this driver; 2.1 shows "Ethernet Built-In" | [mt] |
| Open | `OpenDriver(".ENET0")` gives refnum -48; `.ENET` and `.ENET1` give -43 | [mt] |
| Published speed | "A real DaynaPORT connected to a PowerBook 180 using Fetch gets approximately 80kb/sec. The same machine with a BlueSCSI v2 Pico-W gets 60kb/sec." (a 68030; a Classic will be lower) | [bs] |

### 1.2 Why DaynaPORT 1.2.5 is slow (measured in Snow by MoofTCP, then on hardware)

- **Receive is polled once per tick.** Protocol handlers are entered from a level-1 interrupt
  (VBL / Time Manager level). 20 back-to-back replies arrived "about one per tick, two in the
  same tick at most". That is about **60 frames/s** at best, plus up to 16.7 ms of added
  latency on every inbound frame. [mt]
- **Synchronous transmit takes about one tick.** `PBControlSync` EWrite returns "after about 1
  tick"; 20 back-to-back writes took 18 ticks. Transmit is also capped near 60 frames/s and
  adds latency. [mt]
- **It masks interrupts to level 6** while it delivers frames (SR = 0x2600 in every handler
  call). That adds interrupt latency for the whole machine (mouse, serial, sound). [mt]
- **A synchronous EWrite hangs** if it is called at any interrupt mask above 0, from the
  protocol handler, or from a Time Manager task. [mt]
- With MacTCP's MSS of 536 (observed in Snow) and about 60 frames/s each way, bulk receive was
  estimated to top out near 32 KB/s on the wire. **Superseded by the hardware figures below.** [mt]

**On the real Classic (hw1, hw1b):**

- **At most one delivery per tick.** The spy saw 478 handler deliveries in 478 distinct ticks,
  never two in one tick. That is about **32 full frames/s** (33.5 deliveries/s over 859 ticks of
  bulk receive). The "60 frames/s" figure above came from Snow and does not hold on hardware. [hw1b]
- **Bulk receive is 45.5 KB/s** (46,600 B/s) and **bulk send is 48.7 KB/s** (49,900 B/s) through
  MacTCP with DaynaPORT. The "near 32 KB/s" estimate was based on MSS 536; a real LAN gives MSS
  1,460 (see 1.4), so it is superseded. [hw1]
- **An empty READ costs about 3.5 ms** of bus time (0.21 ticks each). [hw1]
- SpyENET costs about 6% of bulk receive (43,873 vs 46,603 B/s), so spy runs stay out of speed
  tables. [hw1b]

### 1.3 The device protocol (BlueSCSI v2 firmware)

The device answers INQUIRY as vendor `Dayna`, product `SCSI/Link`, revision `2.0f`, device type
3 (processor). It never reselects or interrupts the host, so **the host must poll**. [fw]

| CDB op | Name | Behaviour [fw] |
|---|---|---|
| `0x08` | READ(6) | Allocation length in CDB[3..4]. Each packet is a 6-byte header (`len_hi len_lo 0 0 0 flags`) then `len` bytes. `len` includes 4 trailing CRC bytes, and frames under 60 bytes are padded to 60 first. `flags` = `0x10` means another packet follows *in this READ*. Current firmware (and Snow) clear it on the last packet of every READ even when more frames are still queued, so only old firmware uses it as "queue not empty". The device queue holds at most 19 frames (the 20th makes the ring read as empty). An empty queue returns 6 zero bytes. **CDB[5] bit 6** (`0xC0`) asks for several packets per READ (added 2026-01/03, commits `e7d9629`, `7c20579`); with `0x80` the device sends one. The device pauses **75 µs** between the header and the data, and **300 µs** between packets. In multi-packet mode it stops at the requested length or at about 2 full frames |
| `0x0A` | WRITE(6) | CDB[5] = `0x00`: one packet of CDB[3..4] bytes. CDB[5] = `0x80` (current firmware): a sequence of `[4-byte header: len_hi len_lo x x][len bytes]`, ended by a header with len 0, so **several frames per WRITE**. Older firmware (before `e7d9629`) with `0x80` reads exactly `size + 8` bytes: header, packet, 4 trailing bytes |
| `0x09` | Read MAC + stats | 6-byte MAC, then three 32-bit counters (always zero), 18 bytes |
| `0x0C` | Set interface mode | Ignored |
| `0x0D` | Add multicast | 6 bytes of DATA OUT |
| `0x0E` | Enable / disable | CDB[5] bit 7 = enable; enabling clears the receive queue |
| `0x1A`, `0x40`, `0x80` | Mode sense, set MAC, set mode | Ignored |
| `0x1C` | BlueSCSI Wi-Fi | Subcommand in CDB[1]: scan 1, scan complete 2, results 3, info 4, join 5 |

- **Device receive queue:** 20 packets of 1520 bytes. When it is full, new frames overwrite
  queued ones. [fw]
- **Universal single-frame write:** `[len_hi len_lo 0 0][frame][00 00 00 00]` with CDB[5] =
  `0x80` and CDB[3..4] = frame length works on *both* firmware generations (old: reads
  size + 8; new: reads the header, the frame, then a 0-length header). Batching more than one
  frame needs the new firmware. [fw, derived]
- **Snow** emulates the same command set. READ returns up to 8 packets regardless of bit 6,
  with no gaps; WRITE supports the multi-packet `0x80` format. Snow is good for functional
  tests. It does **not** model SCSI timing, the 75/300 µs gaps, or bus errors from blind
  transfers, so speed must be measured on the real Classic. [snow]

### 1.4 The interface MacTCP uses (System 6 Ethernet driver API)

| csCode | Call | Notes |
|---|---|---|
| 246 / 245 | EAddMulti / EDelMulti | Not used by MacTCP. AppleTalk calls EAddMulti (246) with 09:00:07:FF:FF:FF; the address is eMultiAddr, 6 bytes inline at PB offset 28 (Snow, 2026-11-07) |
| 247 | EAttachPH | `eProtType` + `ePointer` = handler; -94 (`LAPProtErr`) if the type is taken |
| 248 | EDetachPH | |
| 249 | EWrite | `ePointer` = WDS (`{uint16 len; ptr}` entries, ended by len 0). The first piece starts with the destination MAC; the driver fills in the source MAC at +6 |
| 250 / 251 | ERead / ERdCancel | Default handler path; not used by MacTCP (to confirm) |
| 252 | EGetInfo | Dayna writes only our 6-byte MAC |
| 253 | ESetGeneral | MacTCP calls it once after open and needs result 0 (`docs/dev/INTERFACE.md`; same on hardware [hw1b]) |

PB layout: `eProtType`@28, `ePointer`@30, `eBuffSize`@34, `eDataSize`@36 (58-byte PB). The
protocol handler convention is: A3 = just past the 14-byte header, A4 = ReadPacket (ReadRest =
A4+2), D1 = bytes left (Dayna passes frame length − 14 + 4, i.e. it counts the 4 CRC bytes),
A0/A1 driver-private until ReadRest, return with RTS; ReadRest takes D3 = buffer size and A3 =
buffer, returns D3 = 0 / leftover / −truncated and A3 past the last byte. Handlers may call
EWrite from inside the handler (nested). Sources: Inside Macintosh: Networking (EAttachPH,
ERead, multinode receive routine), "Macintosh OS Ethernet Driver, Design Specification
Proposal" (Apple, 1987-05-18, bitsavers), and [mt]'s disassembly and EnetSpike runs.

**Now known (Snow, 2026-10-31; details in `docs/dev/INTERFACE.md`, figures in `docs/dev/BENCH.md`):**
MacTCP finds `.ENET0` by name; it calls open, EGetInfo, ESetGeneral (must return 0) and EAttachPH for IP and ARP; EWrite is always async, up
to 3 WDS pieces, never padded (the driver pads to 60); handlers are IP and ARP only; MSS 536 (max
frame 590); Dayna's footprint is 16,554 bytes (measured on the real Classic in hw1c; Snow's heap walk agrees), so the 16,144 budget
in 2.4 fits with 410 to spare (3 RX + 2 TX at 14,620 leaves 1,934).

**Now known on hardware (hw1b, 2026-11-06; `docs/dev/INTERFACE.md` section 10):**
the call order and csCodes are the same as in Snow, and ESetGeneral is called once after open and
must return 0 [hw1b]. On a real LAN the MSS is 1,460, so frames reach **1,514 bytes both ways**
(RX D1 1,504); the 536/590 figures above are Snow's. Buffers of 1,524 are right [hw1b]. The firmware
pads short frames to 60 bytes (RX D1 50; Snow pads to 64) [hw1b]. Handler SR is 0x2600 on every
delivery, with at most one delivery per tick [hw1b]. `.ENET0` may be opened at INIT time by
AppleTalk/EtherTalk before MacTCP, so the driver must survive that (see
`docs/dev/BOOT-CRASH-ETHERTALK.md`) [hw1b].

**Still OPEN:** what ESetGeneral does; EDetachPH, Close and shutdown behaviour; whether MTU/MSS
depends on anything the driver reports; the
cause of bulk receive ending after 148,912 bytes in Snow.

---

## 2. Design

### 2.1 Packaging

One file, **`MoofPort`** (type `INIT`), dropped into the System Folder. At boot its INIT:

1. Reads preferences (resource in the file: SCSI ID or auto, poll tuning, TX batching on/off).
2. Finds the device: INQUIRY on IDs 0–6 (skipping the boot disk), vendor `Dayna`, product
   `SCSI/Link`.
3. Allocates one locked, non-relocatable system-heap block for driver code and all buffers. Nothing
   is allocated after boot, and nothing at interrupt time.
4. Installs a pointer-based `DRVR` named **`.ENET0`** into the unit table with `DrvrInstall`
   (the pattern MoofTCP's INIT uses for `.IPP`), so MacTCP's `OpenDriver(".ENET0")` finds ours.
5. Shows a parade icon (crossed out if no device was found). Holding Shift at boot skips it.

#### Coexisting with the installed DaynaPORT driver

DaynaPORT stays installed. MoofPort never edits, removes or renames anything in the System file,
and Dayna's driver remains the fallback.

- **Why there is no conflict.** Dayna's driver is only a `DRVR 47` resource in the System file. It has
  no INIT and loads only when something first calls `OpenDriver(".ENET0")`. MoofPort's INIT runs
  at boot, before MacTCP or any app opens the driver, and puts its own `.ENET0` in the unit table.
  `OpenDriver` finds the unit-table entry by name first. This was observed in Snow: a unit-table
  `.ENET0` won over `DRVR 47`, and MacTCP's open reached it (`docs/dev/INTERFACE.md`). So with
  MoofPort installed, Dayna's driver is never opened: it uses no memory and never touches the SCSI bus.
- **The one real hazard is both drivers open at once.** Both would poll the same SCSI target and take
  each other's packets. The INIT guards against it: if a `.ENET0` is already in the unit table or open
  at boot (for example, another driver installed one first), MoofPort does not install itself, shows a
  crossed-out parade icon, and Dayna's driver keeps working.
- **MacTCP needs no change.** It opens `.ENET0` by name, so it keeps "Ethernet Built-In" and its
  address settings. The Ethernet address is the BlueSCSI's own, read from the device, so it is the
  same under either driver.
- **Switching back to DaynaPORT.** Drag `MoofPort` out of the System Folder and restart, or hold
  Shift at boot (the INIT checks for Shift itself, since System 6 does not have a global "extensions
  off"). Nothing else changes.
- **The BlueSCSI Wi-Fi DA** talks to the device directly through the SCSI Manager and not through
  `.ENET0`, so it works the same with either driver. One caveat, from `docs/dev/RESEARCH.md`: the DA
  resets the SCSI bus, so the driver re-enables the device after an unexplained error.
- **Hardware sessions.** Session 1 measures DaynaPORT itself, so it requires DaynaPORT installed and
  MoofPort absent. Session 2 is the first with MoofPort; its checklist confirms on the real Classic
  that MacTCP is running through MoofPort and not Dayna. The proof is MoofPort's counters moving and
  Dayna's driver never appearing in the unit table.
- **Fallback.** If the unit-table route ever fails on real hardware, the fallback below (shipping as
  `DRVR 47` in the System file) needs an installer that renames Dayna's resource reversibly (for
  example to type `DRVx`). That would be the only case where the System file is touched.

Why an INIT and not a System-file `DRVR` like Dayna: drag-to-install, drag-to-remove, Shift to
skip, and it can choose a free unit number and size buffers from prefs at boot. Fallback if
the INIT route hits a wall: the same driver code built as `DRVR 47 ".ENET0"` for the System
file.

### 2.2 The SCSI engine (where the speed comes from)

The Classic has no usable SCSI interrupt and the device never reselects, so the driver polls.
The win comes from polling *at the right moments*, draining everything per poll, and spending
as little CPU and bus time per frame as possible.

- **One poll task, adaptive rate.** A Time Manager task (ms resolution, `PrimeTime`) replaces
  the once-per-tick VBL poll.
  - *Burst:* when a poll returns data, or a frame was just sent, poll again after at least one
    tick (about 8 ms if hardware shows an empty READ is cheaper), tunable, for a short window.
    An empty READ costs about 3.5 ms of bus time on the Classic [hw1], so a 2–4 ms interval would
    spend most of its time on empty READs. Replies to our own frames (ACKs, echoes, the next
    data segment) are still picked up well inside the 16.7 ms Dayna adds.
  - *Idle:* back off step by step to a ceiling (initially 1 tick, tunable up to a few ticks) so
    an idle machine pays almost nothing.
  - The rates are set by measuring on hardware (Phase 4), not guessed.
- **Drain per poll.** The primary path is to drain several frames per poll with multi-packet
  READ (`0xC0`). Dayna delivers at most one frame per tick (about 32 full frames/s) [hw1b], and
  1.5× its receive rate needs about 47 frames/s (section 5), which is more than one frame per
  tick. The firmware supports multi-packet READ (hw1 probe 5). Keep issuing READs until one
  comes back empty (the `0x10` flag only says another packet follows in the same READ; the empty
  one costs about 3.5 ms), up to the free RX slots. Check the bus phase (`SCSIStat`) after
  each packet: if the target is still in DATA IN, another header follows; if it went to
  STATUS, this firmware sent one. That works on old firmware, new firmware and Snow without a
  version check.
- **Split transfers around the device's pauses.** Read the 6-byte header (plus the first few
  data bytes) with handshaked `SCSIRead`, then the rest with `SCSIRBlind`. The 75 µs and 300 µs
  pauses then fall where every byte is handshaked, and the long run of payload goes at blind
  speed. Blind reads and writes on the Classic's 53C80 at the device's pace: **provisional go**
  (hw1b `ScsiProbe-150`: a blind 1,514-byte write gave err=0 with no freeze or bus error; a
  blind payload read gave err=0 with no freeze, but 5 frames / 6,136 bytes in 1 tick, the timer's
  resolution, and the bytes were not compared). Phase 3 has an entry gate for the integrity
  check (section 4). Polled stays the fallback. [hw1b]
  Caveat from `docs/dev/RESEARCH.md`: Apple's DV14 says several `SCSIRead`/`SCSIRBlind` calls
  after one command "may not work", so ScsiProbe tests per-piece calls against one TIB program
  with instruction boundaries at the pauses before the glue commits to either.
- **Transmit immediately.** EWrite tries to send right now (the SCSI Manager is not busy):
  it builds a TIB straight from the caller's WDS (zero-copy), sends the universal
  single-frame WRITE, and completes the PB. If the bus is busy (a disk transfer was
  interrupted) or we are mid-transaction, it copies the frame into a TX slot, completes the PB
  at once, and the poll task flushes the queue at the next chance. Sync EWrite therefore never
  waits for a tick and never hangs, in any context.
- **Batch transmit (firmware v2026.01+, pref, on by default; tested with v2026.08.17).** When more than one frame is
  queued, send them in one multi-packet WRITE and save a selection + command phase per frame.
- **Bus etiquette.** Every transaction starts with `SCSIGet`. If the SCSI Manager is busy, back
  off and retry from the timer, never spin. Transactions are kept short (bounded number of
  frames per READ) so disk I/O is not starved.
- **Later, only if measurements justify it:** drive the 5380 directly for the network
  transactions (after `SCSIGet` claims the bus) to cut SCSI Manager overhead. This is
  Phase 5 work behind its own measurement gate, because it is the riskiest part.

### 2.3 Delivering frames to MacTCP

- Frames are read into a small ring of RX slots (default 3 × 1524 bytes), the SCSI
  transaction is finished, the bus released, and only then are handlers called. A handler that
  calls EWrite (MacTCP sending an ACK from inside its IP input) then finds the bus free and
  sends at once, with no nesting inside a live SCSI transaction.
- Demultiplex on EtherType to the attached handler (IP 0x0800, ARP 0x0806, others as
  attached); frames for unattached types go to the ERead queue or are dropped and counted.
- ReadPacket / ReadRest are tight 68000 assembly (`MOVE.L` / `MOVEM` copy, odd-length and
  odd-address safe), keeping Dayna's register contract and its D1 semantics (including the 4
  CRC bytes) exactly, since MacTCP was tuned against it.
- Handlers are called at the interrupt level the poll task runs at (level 1), **not** with
  interrupts masked to 6.
- A considered and rejected idea: calling the handler while the SCSI data phase is still open,
  so ReadRest reads straight from the bus into MacTCP's buffer. It saves one copy per frame
  but breaks nested EWrite from the handler and holds the bus during MacTCP's processing.

### 2.4 Memory budget

The cap is DaynaPORT's resident footprint (its 12,512-byte code plus whatever it allocates).
**Cap: 17,578 bytes = DaynaPORT's 16,554 + 1,024 (decision 4).** DaynaPORT's 16,554 bytes of
fixed system heap were measured on the real Classic (hw1c, LocalTalk boot,
heap walk; application heap +0) and matching Snow's heap walk. [hw1c] (hw1b's open cost was +0
because `.ENET0` was already open at boot. Snow's older FreeMem figure 15,466 was 1,088 low.)
Working target:

| Part | Bytes |
|---|---|
| Driver code (C + asm, flat Retro68 build) | ≤ 9,300 (0.3d3: 9,260 built) |
| RX ring, 3 × 1524 | 4,572 |
| TX deferral slots, 2 × 1524 | 3,048 |
| State, TIBs, stats, poll state | ≤ 600 |
| **Total** | **≤ 17,520** |

The default is 3 RX + 2 TX (17,520, 58 under the cap). Measured in Snow by the hw1c method
(LocalTalk boot, then HeapInfoOpen of the System-file driver): Phase 2 **16,388** fixed bytes
(166 under DaynaPORT); Phase 3 (adaptive poll, EWrite kick, tuning, verify mode, call log
dropped) **17,304** (750 over DaynaPORT, 274 under the cap). On the real Classic with EtherTalk at boot, hw2
measured +750 against DaynaPORT; the difference to Snow is outside the driver's own blocks
(`docs/dev/BENCH.md` hw2 note 2). Buffer counts come from prefs, so the build can adjust (for
example 2 RX + 1 TX = about 13.5 KB), but decision 4 keeps 3 + 2. The INIT's boot-only code is
released after install. A test (`test_budget`) fails the host build if the configured total
exceeds the cap.

### 2.5 Compatibility rules

- Same driver name, refnum behaviour, csCodes, PB fields, error codes and handler register
  contract as DaynaPORT 1.2.5. Where Dayna's behaviour is a bug (hangs, mask 6), we do the
  sane thing instead, as long as MacTCP can't tell.
- Implement every Ethernet csCode (245–253), including ERead/ERdCancel and multicast, so
  other `.ENET` clients (MoofTCP, the BlueSCSI Wi-Fi DA, possibly EtherTalk later) work too.
- Private Status csCodes for diagnostics: counters, poll-rate state, the device's MAC and
  Wi-Fi info (passing through the firmware's `0x1C` commands), a log ring.
- Must work with and without MultiFinder, with a disk transfer in progress, and with
  `Shift` held at boot.

### 2.6 Code layout (proposed)

```
core/         portable C99, host-tested: device protocol framing (READ/WRITE headers, flags,
              multi-packet parse), RX ring, TX queue, EtherType demux, poll-rate state machine
mac/drv/      DRVR entry stubs (asm), Control/Status dispatch, Ethernet csCodes,
              ReadPacket/ReadRest (asm), SCSI Manager glue, Time Manager task
mac/init/     INIT: prefs, device scan, allocation, DrvrInstall, parade icon
mac/tools/    MoofPortStat (counters and live rate app), EnetBench (raw frame benchmark),
              plus the reused HeapInfo apps
tests/        host tests with a fake SCSI Manager that replays [fw] and [snow] device behaviour
docs/dev/     TOOLCHAIN, EMULATOR, HARDWARE, INTERFACE (measured contract), BENCH (results)
```

The same rules as MoofTCP's `core/`: no `malloc`, no mutable globals, all multi-byte access
through byte helpers, wrap-safe time compares, bounded work per call, ASan/UBSan clean.

---

## 3. Development environment (reused from the sibling projects)

MoofTCP (`../mooftcp`) already built and documented the whole loop on this machine for this
exact Mac model. MoofPort copies its scripts (adapting names and paths) rather than inventing
new ones:

- **Toolchain:** Retro68 in Docker, pinned by digest (`scripts/r68`, `scripts/build-mac.sh`),
  `-m68000`, code resources via `--mac-flat` + Rez (`mac/hello_init` recipe in
  `../mooftcp/docs/dev/TOOLCHAIN.md`).
- **Host tests:** gcc + CMake + ctest, ASan/UBSan, `-std=c99 -Wall -Wextra -Werror`.
- **Emulator:** Snow v1.5.0 (pinned, SHA-256 checked), **Mac Classic ROM, 4 MB**, System
  6.0.8, private Xvfb `:14x` (never the desktop display), DaynaPORT target on SCSI 3, NAT link
  (`scripts/snow.sh`, `deploy.sh`, `fresh-disk.sh`). The golden image (System 6.0.8 + Dayna
  SCSI/Link 1.2.5 + MacTCP 2.1, 10.0.0.2/8) is reused read-only from `$MT_HOME`.
- **Network test services:** `testnet.py` (echo/discard/banner), extended with a bulk source
  and sink for throughput runs.
- **Hardware sessions:** `stage-hw.sh` builds `.bin` + `.hqx` + a plain-language checklist
  to copy over with the BlueSCSI toolbox / shared folder.
- **Rules carried over:** nothing Apple-copyrighted or binary in git; never edit a disk image
  while Snow runs; stop Snow by PID; steps that need sudo or hardware are done by whoever sets up the machine, not by the agent.

---

## 4. Phases

Each phase ends with something runnable and a written result. Speed claims are only made from
hardware measurements.

### Phase 0: Ground truth (Snow + one hardware session)

1. **Bootstrap the repo:** CMake skeleton, CLAUDE.md, the scripts from §3, CI for host tests.
2. **SpyENET:** a pass-through `.ENET0` shim that forwards every call to Dayna's driver and
   logs csCode, sync/async, SR, caller, WDS shape and protocol types into a RAM ring, dumped
   by a small app. Run MacTCP 2.1 through boot, control panel, connect, bulk transfer, close.
   Output: `docs/dev/INTERFACE.md`, the exact contract MacTCP uses.
3. **How MacTCP finds `.ENET0`:** confirm from the spy log (and if needed a look at the 2.1
   `.ipp` / `Enet LAP` bytes) what makes "Ethernet Built-In" appear and which name or unit it
   opens. Confirm that a unit-table driver named `.ENET0` wins over the `DRVR 47` resource.
4. **Memory cap:** measure system-heap use with Dayna vs. without (HeapInfo method from
   `../mooftcp/docs/dev/EMULATOR.md`), at boot, after open, after traffic. This number
   becomes the hard cap.
5. **Baseline benchmark kit:** `EnetBench` (raw frames: poll cost, frames/s each way, echo
   RTT) plus MacTCP-level tests: MacTCP Ping RTT, Flynn/Telnet keystroke echo latency, bulk
   TCP download and upload (Fetch or a small test app against `testnet.py`), and CPU left
   over (a busy-loop counter app) while idle and during transfer.
6. **Hardware session 1:** run the baseline kit with DaynaPORT on the real Classic,
   record the BlueSCSI firmware version, and run the SCSI timing probes: empty-READ cost,
   polled vs blind read/write of a 1514-byte frame, whether blind transfers bus-error at the
   device's pace.

**Exit:** INTERFACE.md, the memory cap (16,554 bytes, hw1c), baseline numbers, and a provisional go on blind transfers (integrity check is the Phase 3 entry gate).

### Phase 1: Minimum working driver (Snow)

`.ENET0` with Open/Close, EGetInfo, EAttachPH/EDetachPH, EWrite (single-frame, polled
transfers), READ polling from a Time Manager task at a fixed 1 tick, handler delivery. INIT
install path. MacTCP 2.1 pings, connects, and transfers through it in Snow.

**Exit:** the Flynn and MacTCP test runs MoofTCP already scripts pass with MoofPort instead of
Dayna.

**Result (2026-11-07):** done in Snow: TcpBench echo 64/64, Flynn connects and echoes through
MoofPort, resident 15,294 bytes (cap 16,554). Hardware not yet run. Details, the two bugs found and
what is not tested: `docs/dev/PHASE1.md`.

### Phase 2: Full compatibility

**Priority: EtherTalk / AppleShare (decision 3).** MoofPort becomes the `.ENET0` AppleTalk opens
at boot and serves EtherTalk and MacTCP at the same time through one poll loop:

- System-file install: MoofPort as the `DRVR 47 ".ENET0"` resource in the System file, DaynaPORT's
  resource renamed reversibly (for example to type `DRVx`), with an installer/uninstaller app. The
  INIT stays for MacTCP-only setups (it stands aside when `.ENET0` is already open).
- Open at INIT time must work: no `CurStackBase`, `CurrentA5` or other application globals in Open
  or the poll path (`docs/dev/BOOT-CRASH-ETHERTALK.md`).
- What EtherTalk needs that MacTCP does not: real multicast (EAddMulti/EDelMulti programming the
  device with `0x0D`), delivery of 802.3 length-field frames to the type-0 handler (802.2/SNAP),
  AARP (0x80F3) and whatever else the capture shows (possibly ERead, Apple's Ethernet shell
  `DRVR 127 ".ENET"` on the real Classic's disk). First step: capture AppleTalk's real calls in Snow.
- Snow testing uses a MoofPort-only work image with EtherTalk Phase 2 built from the real Classic's disk,
  kept in `$MP_HOME` and never in git (MoofTCP's golden image has no AppleTalk). Whether Snow's
  NAT link carries AppleTalk to anything useful (netatalk on the host) is to be found out; if not,
  AppleShare is tested in a hardware session.
  **Found out (2026-11-07):** Snow's NAT link does not carry AppleTalk, but a TAP link does
  (`MP_ETH_LINK=tap:<dev>`, netatalk's atalkd on the TAP). AppleShare over EtherTalk through
  MoofPort passes in Snow: AfpCopy's 336 KB round trip, byte-exact (`docs/dev/AFPCOPY.md`).
  **On hardware (hw2, 2026-11-07): passes.** MoofPortInstall put MoofPort into the real Classic's
  System file; AppleShare to a real server and MacTCP both worked through it, AfpCopy
  byte-exact at 54 / 40 KB/s against DaynaPORT's 21 / 18 KB/s. The same-time run with MacTCP
  traffic could not be done (TcpBench does not yield under MultiFinder; tool fix first).

Also: all csCodes (ERead/ERdCancel), sync EWrite from every context, busy-bus deferral, disk I/O
during traffic, MultiFinder on and off, Shift-to-skip, no-device boot, the BlueSCSI Wi-Fi DA
working alongside, MoofTCP's ApiProbe/MoofDiag runs on top of MoofPort.

**Exit:** a compatibility matrix in `docs/dev/` with every row passing in Snow, including an
AppleShare row (mount a volume and copy a file with MacTCP running at the same time; on hardware
if Snow cannot carry AppleTalk).

### Phase 3: Fast path

Adaptive polling, drain per poll with phase detection, split polled/blind transfers, immediate
zero-copy transmit, asm ReadRest, optional TX batching.

**Entry gate (blind transfers):** before Phase 3 relies on blind transfers, a verifying blind
probe must pass on hardware: compare the bytes against a polled read, over at least a few hundred
full frames. Until it passes, the go from Phase 0 is provisional and polled transfers stay the
path. [hw1b]

**Exit:** Snow regression suite still green; `EnetBench` shows the expected reduction in polls
and copies.

**hw3 (2026-11-07):** the entry gate passed: with blind payload reads and every received
IPv4 frame's checksums checked, 697 frames of 1,000+ bytes (1,605 in all) arrived with 0 bad,
no bus errors, no freeze. The adaptive poll did not pay off: burst 5 ms is no better than one
poll per tick, burst 3 ms cuts TCP send to a third. Each empty READ costs about 3.5 ms of CPU,
so polling more often takes CPU from MacTCP. **Gate passed; decision 5:** driver 0.3d2 polls
once per tick and reads payloads blind by default. Next: the cost of the empty poll (a timing
probe in ScsiProbe, then Phase 5).

### After hw4: proposal (2026-11-07, not started; to be confirmed)

hw4 split one empty READ: about 3.3 ms, of which about 1.2-1.3 ms is the BlueSCSI answering
selection (the same on the raw 53C80 path), 0.2-0.3 ms arbitration, and about 1.3 ms the
command, header, status and message handshakes with the SCSI Manager's own work. A raw 53C80
path in C was 3x slower than the SCSI Manager; it works, but would need tight assembly to win,
and could save at most about 1 ms per poll. In order of expected gain:

1. **Blind TX payload writes** (behind a MoofTune flag, then default if clean). Send is the main
   miss (0.67-0.77x DaynaPORT). Every EWrite is one WRITE whose 1,514-byte payload goes polled,
   about 11.8 ms per full frame at hw1's 128 KB/s polled rate; blind reads lifted receive by
   22-24%. Verification on the host: TCP segments with bad checksums are dropped by the PC's
   kernel and counted (`/proc/net/snmp` InCsumErrors), and the TcpBench sink counts bytes.
2. **Idle back-off**: an idle Mac spends about 3.3 ms of every 17 ms tick (about 20% of the CPU)
   on empty polls, as DaynaPORT does. Back off to 34-50 ms after a quiet spell, back to one tick
   on any traffic; measure idle CPU (CpuMeter) and echo after idle.
3. **Selection latency** is the BlueSCSI firmware's: worth asking upstream whether the 1.2 ms
   answer to selection can be shortened (a decision outside this repository).
4. **Raw 53C80 poll in assembly** (Phase 5): only after 1 and 2, as it can save at most about 1 ms
   of the 3.3.

### After hw6: proposal (2026-11-07; applied 2026-11-08 as decision 6)

1. **Defaults for 1.0:** burst 5 ms for 60 ms after traffic (receive +9%, no cost seen) and idle
   back-off to 3 ticks after 2 s quiet (fewer empty polls, more idle CPU), blind reads on, blind
   writes off (safe but no gain). That is MoofTune `burst` plus `idle` in one setting.
2. **The send gap (about 0.8x DaynaPORT) is not the poll rate or the payload transfer** (hw5,
   hw6). One raw test settles whether it is the driver's per-WRITE cost or MacTCP's pacing:
   EnetBench's EWrite rate (hw1 DaynaPORT: 45 frames/s) with MoofPort. If MoofPort's raw rate is
   well above what TcpBench needs, the gap is MacTCP-side and stays; if not, the per-WRITE cost
   (selection 1.2 ms + command, preamble and status) is next.
3. **Then Phase 6, release prep:** the 1.0 defaults built in, `vers` 1.0, a user README (install,
   update, uninstall, recovery), the compatibility matrix, the release-candidate hardware
   session (memory check, a day of normal use).

### Phase 4: Hardware tuning

**Hardware session 2:** the same benchmark kit as Phase 0 with MoofPort, plus a sweep of the
poll-rate settings. Pick defaults from the data. Stress runs: long transfers, disk copy during
a transfer, Wi-Fi reconnect.

**Exit:** `docs/dev/BENCH.md` comparing DaynaPORT and MoofPort, and defaults chosen.

### Phase 5 (optional): Direct 5380 access

Only if Phase 4 shows SCSI Manager overhead is a large part of the per-frame cost. Same tests,
same exit bar.

### Phase 6: Release

Install/uninstall notes, a `vers` resource, README for users, **hardware session 3** (release
candidate, memory check, a day of normal use).

---

## 5. Targets

Set from the Phase 0 baseline (hw1). Starting targets on the Classic, against
DaynaPORT 1.2.5 on the same Mac, same Wi-Fi, same MacTCP 2.1:

| Metric | Target |
|---|---|
| Added receive latency from polling | ≤ 4 ms while active (Dayna: up to 16.7 ms) |
| Transmit latency (EWrite call to frame on the bus) | under 2 ms when the bus is free (Dayna: about one tick) |
| MacTCP Ping RTT to the gateway | at least 15 ms lower on average |
| Bulk TCP receive through MacTCP | at least 1.5× Dayna: Dayna's baseline is 45.5 KB/s [hw1], so about 68 KB/s (roughly 47 full frames/s), and limited by MacTCP's CPU use rather than the driver. Re-check after hw2: it may be near what MacTCP's CPU cost allows on a Classic |
| Interrupt masking | never above the level the poll task already runs at (Dayna: level 6) |
| Idle CPU cost | no more than Dayna's |
| Resident memory | ≤ Dayna's + 1,024 bytes, measured the same way (decision 4) |

MacTCP's own limits (MSS 536 in Snow, 1,460 on a real LAN, window about 4 KB, its CPU cost per segment) will cap bulk
throughput no matter how fast the driver is. The driver's job is to stop being the bottleneck.
MoofTCP on top of MoofPort is where the larger bulk gains will show.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| Blind SCSI transfers bus-error or corrupt data on the Classic at the device's pace | Provisional go from hw1b (blind write and read ran clean, bytes not yet compared); integrity check is the Phase 3 entry gate; split header/data reads; polled fallback is always built in |
| MacTCP 2.1 finds the Dayna driver by something other than the name | The spy log shows it; fallback is shipping as `DRVR 47` in the System file |
| Faster polling steals CPU from MacTCP and apps | Adaptive back-off; CPU cost is a measured target, not an afterthought |
| Older BlueSCSI firmware lacks multi-packet READ/WRITE | Phase detection on READ; universal single-frame WRITE; batching can be turned off in prefs |
| Snow's timing is idealized, so emulator speed results mislead | Speed numbers come only from the hardware sessions |
| Memory cap is tighter than expected | Buffer counts are prefs; the budget test enforces the cap. Cap is measured: 16,554 (hw1c) |
| Interrupt-time SCSI while the File Manager owns the bus | `SCSIGet` first, defer on busy, never spin |

---

## 7. Decisions (2026-10-31 and 2026-11-07)

1. **Hardware sessions: approved.** Three short sessions on the real Classic (Phase 0
   baseline, Phase 4 tuning, Phase 6 release), unlike MoofTCP, which tests hardware only at
   the release candidate. Speed can't be measured in Snow.
2. **Firmware: BlueSCSI v2 `v2026.08.17`.** That tag contains both multi-packet commits
   (`e7d9629` READ/WRITE batching, `7c20579` the CDB[5] bit 6 gate), and its `network.c`
   matches the version read for this plan except for 3 added lines. So multi-packet READ and
   batched WRITE are the **primary** path on the test hardware, and TX batching defaults to **on**.
   The single-packet paths stay as the fallback for older firmware.
3. **EtherTalk / AppleShare is required (2026-11-07).** "I do need EtherTalk support for
   AppleShare." MoofPort must serve AppleTalk over EtherTalk and MacTCP at once. Phase 2 adds the
   System-file install (MoofPort as `DRVR 47 ".ENET0"`, DaynaPORT's resource renamed reversibly,
   installer/uninstaller app), multicast, 802.2/SNAP and AARP delivery, and an AppleShare row in the
   exit matrix. The INIT install stays for MacTCP-only setups.
4. **Memory: performance first, up to about 1 KB over DaynaPORT (2026-11-07).** "I'm not too
   concerned about 750 bytes, if performance would be negatively impacted by changing that."
   After hw2 measured +750 fixed bytes against DaynaPORT (EtherTalk boot), the hard rule
   "resident memory <= DaynaPORT's" becomes "at most DaynaPORT's 16,554 + 1,024 bytes". Only trims
   with no performance cost (dead code, state packing); the 3 RX + 2 TX slots stay.
   `test_budget`'s cap is 17,578.
5. **Poll timing and blind reads (2026-11-07, after hw3).** Decided: "Per tick + blind
   reads on". Driver 0.3d2 polls once per tick with no burst after traffic (hw3: the 5 ms burst
   did not help and a 3 ms burst cut TCP send to a third, because every empty READ costs about
   3.5 ms of CPU), and reads frame payloads blind by default (the header and the device's pauses
   stay handshaked). MoofTune keeps the other presets for experiments.
6. **1.0 defaults and release (2026-11-08, after hw6).** Decided: "Burst + idle, start
   release". MoofPort 1.0b1 polls every 5 ms for 60 ms after traffic, then once per tick until
   2 s of quiet, then backs off to three ticks; blind payload reads on, blind frame writes off.
   The core's `mp_poll` gained the middle level (`mid_ms`, `quiet_ms`). Phase 6 (release prep)
   starts: user README, compatibility matrix (`docs/dev/COMPAT.md`), `scripts/release.sh`, and
   the release-candidate session hw7.

---

## 8. Sources

- Hardware results (real Mac Classic): hw1 and hw1b, summarised in `docs/dev/BENCH.md` (raw
  session logs are kept outside the repository)
- BlueSCSI WiFi DaynaPORT guide: https://bluescsi.com/docs/WiFi-DaynaPORT
- BlueSCSI v2 firmware: https://github.com/BlueSCSI/BlueSCSI-v2 (`lib/SCSI2SD/src/firmware/network.c`, `network.h`; `src/BlueSCSI_config.h` for INQUIRY strings)
- Snow emulator DaynaPORT model: https://github.com/twvd/snow (`core/src/mac/scsi/ethernet.rs`)
- Inside Macintosh: Networking, Ethernet driver chapter (EAttachPH, ERead, protocol handlers):
  https://developer.apple.com/library/archive/documentation/mac/Networking/Networking-271.html,
  `.../Networking-275.html`, `.../Networking-289.html`
- "Macintosh OS Ethernet Driver, Design Specification Proposal", Apple, 1987-05-18 (bitsavers)
- MoofTCP's measured `.ENET0` contract, memory method and pipeline: `../mooftcp/docs/dev/`
  (`MACTCP-INTERFACE.md`, `EMULATOR.md`, `GOLDEN-IMAGE.md`, `TOOLCHAIN.md`, `HARDWARE.md`)

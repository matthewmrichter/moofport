# MoofPort research notes (Phase 0, desk research)

Status: desk research only. No hardware or emulator was available when this was written.
Every fact carries a source. **OPEN** marks what no source settled, with the experiment that
would settle it. Tags: **[IM-D]** Inside Macintosh: Devices; **[IM-N]** Inside Macintosh:
Networking; **[DV06]/[DV14]/[DV23]/[DV16]** Apple Technical Notes/Q&As; **[mt]** MoofTCP's
measurements (`../mooftcp/docs/dev/MACTCP-INTERFACE.md`); **[fw]** BlueSCSI v2 source at
`v2026.08.17`-era `network.c`; **[snow]** Snow emulator source (commit `fbdcbac`).

Source URLs (all fetched 2026-10-31 unless noted):

- IM-D, Device Manager: https://developer.apple.com/library/archive/documentation/mac/Devices/Devices-23.html and `-24.html`
- IM-D, original SCSI Manager: `.../Devices/Devices-119.html` (intro), `-121`, `-122` (polled vs blind), `-138` (SCSIGet), `-139` (SCSISelect), `-141` (SCSICmd), `-144/145/146/147` (Read/RBlind/Write/WBlind), `-148` (SCSIComplete), `-149` (SCSIStat), `-150` (summary, error codes)
- DV06: https://developer.apple.com/library/archive/technotes/dv/dv_06.html
- DV14 (SCSI Bugs): https://developer.apple.com/library/archive/technotes/dv/dv_14.html
- DV16: https://developer.apple.com/library/archive/qa/dv/dv16.html
- DV23 (Driver Education): https://developer.apple.com/library/archive/technotes/dv/dv_23.html
- IM-N Ethernet: `.../mac/Networking/Networking-262.html` (protocol handler), `-271` (EAttachPH), `-274` (EWrite), `-275` (ERead), `-278` (EGetInfo), `-279` (ESetGeneral), `-281` (EAddMulti)
- Time Manager: `.../mac/Processes/Processes-56.html`, `-60`, `-62`, `-68`, `-73`
- Apple, "Macintosh OS Ethernet Driver, Design Specification Proposal", 1987-05-18: https://www.bighole.nl/pub/mirror/www.bitsavers.org/pdf/apple/nubus/apple/670-0205_3Com_Ethernet/Macintosh_OS_Ethernet_Driver_19870518.pdf
- MacTCP Programmer's Guide 1989: https://kuiper.mirrorservice.org/sites/www.bitsavers.org/pdf/apple/mac/developer/Networking/MacTCP_Programmers_Guide_1989.pdf
- NW540 tech note: https://developer.apple.com/library/archive/technotes/nw/nw_540.html

Not used on purpose: the Apple System 7.1 source that circulates on git mirrors (e.g.
`OS/SCSIMgr/SCSIMgrOld.a`, `OS/TimeMgr/TimeMgr.a`). It is copyrighted Apple code; the project
rule is that nothing Apple-copyrighted enters the repo, and we keep the research clean too.
The documents above were enough for the decisions below; the rest is routed to experiments.

---

## 1. Device Manager on System 6: how `.ENET0` can be installed

### What is established

- **`OpenDriver`/`PBOpen` search the unit table first.** [IM-D Devices-23/-24]: "To avoid
  replacing an open driver, the Device Manager searches the drivers that are already installed in
  the unit table before searching driver resources. If the specified driver is already open, this
  function simply returns the driver reference number." Only if it is not found does it call
  `GetNamedResource(name, 'DRVR')`, and then the resource ID is the unit number. Names are
  compared case-insensitively (not diacritical marks), 1 to 255 characters after the period.
  Caveat: these pages are the 1994 edition of the book. Whether the System 6.0.8 / 256K-ROM `_Open`
  does the same is **not documented in a System 6 era source**. Indirect support: DV23 (1990)
  tells INIT authors to "mimic" `_Open`'s logic by searching the table for a name match, and says
  that if the slot is taken "the driver that is already there remains there". **OPEN-1** see below.
- **Table layout.** [DV23] `UTableBase` ($11C) points to a non-relocatable system-heap block of
  handles to DCEs; `UnitNtryCnt` ($1D2) is the entry count; slot index = unit number; refnum =
  `~unit` (-1 - unit). A nil handle means no driver. [DV06] walks the table by `UnitNtryCnt` and
  compares the name at DCE offset `$12` (the driver header's name, via `dCtlDriver`), starting at
  refnum -12.
- **Reserved ranges.** [DV23] Units 0-11 serial/disk/AppleTalk/printer; 12-31 desk accessories;
  32-39 SCSI (ID n at unit 32+n); 40-47 AppleShare and other Apple use; 48-63 and up slot drivers,
  "open for other drivers as well". DaynaPORT's `DRVR 47` (refnum -48) is in the Apple-reserved range
  and not the free range. [mt]
- **Table size by model.** [DV23] "On the Macintosh 512KE and Macintosh Plus, the unit table does
  not go past unit number 47. On the Macintosh SE, the unit table is large enough to hold up to
  unit number 63. On the Macintosh II family ... up to 127." DTS advice: if `UnitNtryCnt` was
  48, grow to 64; the system grows by 4 at a time.
- **Free slot search.** [DV23] Start at unit 48 and scan to `UnitNtryCnt` for a nil handle; if none,
  grow the table: `NewPtr` (system, clear) a bigger block, copy the old table, then update
  `UTableBase` and `UnitNtryCnt`, with interrupts off, and check the `NewPtr` result. Apple's
  sample TradDriverLoaderLib does the copy in this order: copy, switch base pointer, then raise the
  count (https://developer.apple.com/library/archive/samplecode/TradDriverLoaderLib/Listings/MoreDevices_TradDriverLoaderLib_TradDriverLoaderLib_c.html).
  The same sample first rejects a duplicate name (returns `dupFNErr`), then finds a free unit.
- **`DrvrInstall` for a pointer-based driver.** [DV23]: after finding a free slot, call
  `_DriverInstall` with the refnum and a pointer to the driver; this creates the DCE and sets
  `dCtlRefNum`. DV23's follow-up steps: put the driver (handle) in `dCtlDriver`, copy
  `drvrFlags/Delay/EMask/Menu` into the DCE, set `dRAMBased`. The resource must have the system
  heap attribute, be detached with `_DetachResource`, and have a `'sysz'` resource if over 16K.
  [TradDriverLoaderLib sample] adds: `DriverInstall` "seems to ignore the first parameter", so
  write `dCtlDriver` yourself; for a **pointer** driver clear the `dRAMBased` bit and set
  `dNeedLock`; `DriverInstallReserveMem` (128K ROM and later) allocates the DCE low in the heap.
  [DV06]: pointer drivers exist already (SCSI drivers live in RAM, their DCEs hold pointers).
- Per [IM-D Devices-23], `OpenDriver` "may move memory; you should not call it at interrupt time".

### What this means for MoofPort

The plan's Phase 0 item 3 is plausible and is the standard documented route: an INIT (or the
driver loader) installs a DRVR named `.ENET0` into a free unit (search from 48 up; grow the table
if needed; SE-class hardware only guarantees 63) and `OpenDriver(".ENET0")` then returns that
refnum without ever touching the `DRVR 47` resource. The risk is ordering: if Dayna's resource is
opened first (by anything), its entry holds the name and ours is rejected (DV23: "the driver that
is already there remains there"). So MoofPort's INIT must run before MacTCP ever opens `.ENET0`
(INITs run at startup, and MacTCP opens its driver later; the Dayna driver itself is not opened
by an INIT) and should deal with the already-installed case by reporting it. Prefer the simplest
fallback the plan already has: ship as `DRVR 47` in the System file. For the unit number, do
not take 47 unless the free-slot search fails; take the first nil slot at or above 48 and grow the
table by 4 only if necessary. Remember that `OpenDriver` must not be called at interrupt time.

### OPEN

- **OPEN-1: does the 6.0.8 `_Open` search the unit table by name before the DRVR resource?**
  Experiment (Snow, 6.0.8 Classic ROM): install a pointer driver named `.ENET0` into unit 48 from
  an INIT, then call `OpenDriver(".ENET0")` and check refnum -49 vs -48. Add a run with both
  present. This is a Phase 0 item (3) and costs nothing.
- **OPEN-2: `UnitNtryCnt` on the Mac Classic with 6.0.8** (DV23 gives SE = 64 entries; it names
  no Classic). Experiment: read `$1D2` in an app and dump nil/non-nil slots. Also check that growth
  works (call the grow routine, verify no crash).
- **OPEN-3: exact DCE field offsets and `_DriverInstall` register interface on the 256K/512K ROM**
  (documents above describe it only in prose). Take them from the Mac OS 7 interface headers
  used by Retro68 (`DCtlEntry` in `Devices.h`) at build time; do not copy them into the repo.

---

## 2. The original (pre-4.3) SCSI Manager on the Classic

### Call semantics

- `SCSIGet` "prepares the SCSI Manager to initiate the arbitration sequence. If the SCSI Manager
  is busy with another operation, this function returns the `scMgrBusyErr` result. If arbitration
  failed because the bus was busy, the function returns the `scArbNBErr` result." On some models
  arbitration does not happen until `SCSISelect`; you must still call `SCSIGet` first. [IM-D Devices-138]
- `SCSISelect` requires a prior `SCSIGet`; result codes `scArbNBErr` (3), `scCommErr` (2),
  `scSequenceErr` (8). [Devices-139]
- `SCSICmd` sends a 6, 10 or 12 byte CDB. A data transfer call must follow a `SCSICmd`. [Devices-141, -127]
- `SCSIRead`/`SCSIWrite` are polled; `SCSIRBlind`/`SCSIWBlind` "do not poll the SCSI controller
  before transferring each byte ... polled only for the first byte transferred by each `scInc`,
  `scNoInc`, or `scComp` TIB instruction." [Devices-145..147]
- **TIB opcodes** (`scOpcode`, 16-bit; params two longs): `scInc`=1, `scNoInc`=2 (transfer;
  param1 = buffer, param2 = count), `scAdd`=3, `scMove`=4, `scLoop`=5 (param1 = relative branch,
  param2 = counter), `scNop`=6, `scStop`=7, `scComp`=8 (compare; not supported in 4.3). Example from
  the book: `scInc $67B50 512; scLoop -10 6; scStop` = six 512-byte blocks. [Devices-121, -127, -135]
  Per-opcode parameter meaning is given in the book only for the examples cited (Devices-127 gives
  names only); the layout above for `scInc/scNoInc/scLoop/scComp` comes from the sample TIB and the
  `scComp` page. Mark `scAdd/scMove` parameter meaning as unverified; MoofPort does not need them.
- `SCSIComplete(stat, message, wait)`: `wait` is in **ticks**; "must be called at the end of each
  SCSI transaction, even if the transaction does not complete successfully"; returns `scCommErr`
  on timeout; `scComplPhaseErr` (10) when it had to bit-bucket or send $EE bytes to reach status
  phase. [Devices-148, -122]
- `SCSIStat` returns 16 bits: bits 0-9 bus signals (complement, active low), 10-15 controller
  status (BSY ERR, PHS MAT, INT REQ, PTY ERR, DMA REQ, END DMA). Apple says "use of this function
  is not recommended" because of hardware differences. [Devices-149] DV14: broken before System
  file 3.2, so fine on 6.0.8; the existing bug list notes `SCSIStat` as the way to test for bus free
  before VBL SCSI.
- Error codes: `scCommErr` 2, `scArbNBErr` 3, `scBadParmsErr` 4, `scMgrBusyErr` 7, `scSequenceErr` 8,
  `scBusTOErr` 9, `scComplPhaseErr` 10. [Devices-150]

### Known bugs and limits that matter [DV14 unless noted]

- The Manager is **not asynchronous and not reentrant**: "does not currently support
  interrupt-driven (asynchronous) operations"; the SE has a maskable interrupt for IRQ (no async
  support in the OS). Disconnect/Reselect routines "cannot be used".
- **Interrupt-time use** (VBL/Time Manager): Apple's text is a warning, not a ban. A third-party
  driver issued SCSI from a VBL task without checking that the bus was free; Apple recommends
  avoiding SCSI calls from interrupt handlers and, if you must, to call `SCSIStat` first and check
  every error, because "SCSI operations during VBL are not guaranteed". Apple will not be
  compatible with products that bypass the Manager.
- `SCSIGet` "will fail if the BSY line is still asserted" (fixed to wait up to 200 ms in 4.1/4.2):
  retry `SCSIGet` once or twice. The selection timeout was 900 µs before 4.1/4.2 fixes, then 250 ms.
- Polled reads and writes **have no timeout** in the 3.2 Manager: a device that stalls hangs the
  host. The last byte of a write is not verified in pseudo-DMA mode in 3.2 (bus can hang in Data
  Out); fixed in 4.1/4.2 ("checks for a final DRQ or a phase change").
- **Multiple `SCSIRead`/`SCSIRBlind` calls after one command and before `SCSIComplete` "may not
  work"**; the first byte of the second transfer can be lost; "will probably not be fixed". This
  matters for the plan's header-then-payload split: **use one TIB program per command** (multiple
  TIB instructions in one call), never two `SCSIRead` calls per command. (Pre-3.2 "dropped a byte on
  two or more reads in a row" is fixed in 3.2.) Consistent with the book's own advice to split with
  TIB instructions, not calls.
- `SCSIComplete` can return a false `scComplPhaseErr` on the SE and II if a slow target is still in
  Data In when it checks. Treat `scComplPhaseErr` as a warning (data valid) for this device, but
  log it.
- On the SE and II, a wrong byte count in a blind transfer returns an error; on the Plus it
  silently over/underruns.

### Blind transfers: the timing rule

- [IM-D Devices-122] "Newer Macintosh models include hardware support for handshaking ... This
  handshaking allows the SCSI controller to defer the CPU if no data is available. If the data
  doesn't arrive within a specified period, the SCSI Manager returns `scBusTOErr`. The timeout
  period varies for each Macintosh model." "Because the first byte transferred by each TIB
  instruction is always polled, even in blind mode, you can work around predictable delays using
  an appropriate sequence of TIB instructions ... divide the transfer into blocks so that the delayed
  byte is located at the start of a TIB instruction." (This is exactly the plan's split design.)
- [DV16] The blind design "presumed that the device could move data across the bus with no more
  than a 16-microsecond latency between bytes."
- [fw] The BlueSCSI firmware sleeps **75 µs** after the 6-byte header and **300 µs** between packets,
  with the comment that "DaynaPort Mac driver needs a short delay ... Timing matches real DaynaPORT
  SCSI/Link-3 behavior observed on a SCSI bus analyzer." (`network.c` lines ~317 and ~330.)
  Both pauses are many times longer than 16 µs, so any blind run that spans them would exceed the
  stated tolerance. Related: SCSI2Pi issue #5 (https://github.com/uweseimet/scsi2pi/issues/5):
  "The MacOS driver is broken and requires a delay when data (a network packet) are returned";
  the driver is recognised by its 17-byte INQUIRY.

### Throughput figures (weak)

- Low End Mac, "SCSI Throughput" (https://lowendmac.com/1998/scsi-throughput/): Plus 2,104 Kbps,
  SE 5,248 Kbps (units and method not stated; no Classic figure). Blind transfers "improve SCSI
  throughput by about one-third compared with Apple's standard driver" (Plus, SCSI Accelerator).
  Inference, not a source claim: at the ~60 frames/s the old driver reaches, even the Plus figure
  above is far higher than the traffic, so per-command overhead and polling interval, not raw
  bus speed, look like the limits. Phase 0 hardware timing (OPEN-6) must confirm it.
- BlueSCSI's published DaynaPORT speed (80 vs 60 KB/s on a PowerBook 180) is in the plan (§1.1).

### What this means for MoofPort

1. Always `SCSIGet` first; treat `scMgrBusyErr` and `scArbNBErr` as "defer", retry from the timer, do
   not spin. One retry on `scArbNBErr` immediately is acceptable (DV14).
2. Use a single TIB program per transaction. For reads: `scNoInc/scInc` header and first bytes
   (polled start), then `scInc` for the payload (polled first byte, blind rest). The first byte of
   every TIB instruction is polled, so each long device pause must sit exactly at an instruction
   boundary: before byte 7 (75 µs) and at the start of the next packet's header (300 µs). Place a
   TIB boundary at each. The part between pauses can then be blind.
3. Unknown timeout for `scBusTOErr` on the Classic. Because a bus timeout on the Classic is raised as
   a bus error by the handshake logic (NetBSD: "the general logic IC ... will generate a /BERR for us
   in short order", https://nxr.netbsd.org/xref/src/sys/arch/mac68k/dev/sbc.c?r=1.20), a mistake costs
   a SCSI Manager error, not necessarily a crash, but build the polled fallback first.
4. Always finish with `SCSIComplete` (ticks; use a short wait such as 2-3) and on any error also
   consider `scComplPhaseErr` data valid.
5. From interrupt time (VBL/TM): only `SCSIGet` then proceed; check `SCSIStat` for BSY/SEL first as
   DV14 suggests; never after the File Manager has called `SCSIGet` (it returns `scMgrBusyErr`).
   Bounded transactions only.

### OPEN

- **OPEN-4: the Classic's `scBusTOErr` timeout and what a blind read does at a 75 µs / 300 µs gap.**
  Experiment (hardware session 1, in the plan): four loops of 1000 reads of a 1514-byte frame:
  (a) all polled; (b) one blind run; (c) header polled, payload blind with a TIB boundary at the pause;
  (d) multi-packet with a TIB boundary at each gap. Record result codes, bus errors, and byte-compare
  every frame. A test frame generator is the device's loopback or a ping flood.
- **OPEN-5: scMgrBusyErr vs File Manager.** Whether `SCSIGet` from a Time Manager task while a disk
  read is mid-transaction returns `scMgrBusyErr` or hangs on a Classic with 6.0.8. Experiment:
  loop file reads in the foreground, a 1 ms TM task calling `SCSIGet`/`SCSIComplete`, count results.
- **OPEN-6: SCSI polled/blind throughput on a Classic** (no source found). Experiment: time reading
  a 1514-byte frame, polled vs blind, with `Microseconds` unavailable (see section 4): use the VIA
  timer or a loop over many reads and `TickCount`.

---

## 3. Classic's NCR 53C80 (for a later direct-5380 option)

### What is established

- Address window: SE and Classic decode the SCSI chip at `$580000-$5FFFFF` (the Plus too, when it
  has SCSI). [snow `core/src/mac/compact/bus.rs`, lines ~1041, ~1067]
- Register select: `reg = (addr >> 4) & 7`; address bit 0 distinguishes read/write cycle (odd
  address = write); address bit 9 (`0x200`) is DACK, i.e. the pseudo-DMA data window. [snow
  `core/src/mac/scsi/controller.rs`, lines ~821-875.] The chip is on the upper byte lane. [snow `bus.rs` ~411]
- IRQ: only the SE and later route 5380 IRQ to the CPU, as level 1 via VIA, gated by VIA port B bit 6
  ("SCSI interrupt, false = enabled (SE+)"). [snow `bus.rs` ~974, `via.rs` ~96.] Matches [DV14]: "The
  Macintosh Plus can never support it since there is no interrupt capability ... The SE has a
  maskable interrupt for IRQ".
- DRQ: on the compact Macs it is not an interrupt. MAME's `macscsi.cpp` header says DRQ is "wired
  onto DTACK (68000)" so wait states match SCSI timing. (https://git.redump.net/hbmame/tree/src/mame/machine/macscsi.cpp,
  secondary source.) In a Mac II family machine, IRQ and DRQ do go to VIA2 (CB2/CA2). [snow `macii/bus.rs` ~1026]
- Handshake logic: NetBSD's sbc driver says Macs after the Plus with a 5380 have a "general logic
  IC" that handshakes blind transfers; if the SCSI side is not ready, the IC "will generate a /BERR for
  us in short order"; the fault address minus the transfer base gives bytes actually moved
  (sbc.c `sbc_drq_intr` comments). It cites "Guide to the Macintosh Family Hardware, pp. 137-143" as
  the register source.
- Linux `mac_scsi.c` (https://codebrowser.dev/linux/linux/drivers/scsi/mac_scsi.c.html) treats a faulting
  `MOVE.B` on receive as "the target sent nothing; try again" and retries the whole command on send;
  word moves cannot be retried; pseudo-DMA chunks are 512 bytes; its header quotes Inside Macintosh
  on a byte-delay limit "to prevent bus errors when a target fails".
- 5380 register numbers as used by Linux: `STATUS`=4, `BUS_AND_STATUS`=5, `INPUT_DATA`=6, `OUTPUT_DATA`=0;
  BSR bits PHASE_MATCH 0x08, IRQ 0x10, DRQ 0x40.
- Apple's own notes: "The Macintosh SCSI implementation does not support arbitration, so only the
  Macintosh can start a SCSI transaction" (HW540,
  https://wiki.preterhuman.net/images/0/01/HW540-SCSI_Port_Q%26As.pdf), and write directly to
  the chip at your own risk (the 5380 requires the driver to track bus phases).

### What this means for MoofPort

Direct 5380 use is feasible at addresses `$580000..` via the usual compact-Mac layout, but nothing
here is hardware-verified. Do not do it in Phases 1-4. If Phase 5 happens: claim the bus with
`SCSIGet` first (so the OS knows), keep IRQ disabled (we never need it: the device never reselects),
use polled register access (cheap on a 68000) and treat any pseudo-DMA window use as the same hazard
as `SCSIRBlind`. The tangible benefit is skipping the SCSI Manager's per-call overhead, not
throughput. No benefit has been measured, so keep this behind the Phase 5 gate.

### OPEN

- **OPEN-7: register map confirmation for a real Classic.** Source is the emulator plus drivers for
  other models. Experiment: in an app on a real Classic, `SCSIGet` then read chip registers `$580000+(n<<4)`
  and compare to `SCSIStat`.
- **OPEN-8: Classic-specific differences from the SE** (the Classic ROM is 512K and some SCSI code differs).
  Not covered by any source here. Same experiment.

---

## 4. Time Manager on System 6.0.8 / Classic

### What is established

- **Revised Time Manager arrived in System 6.0.3.** [Processes-56] "System software version 6.0.3
  introduced a revised version of the Time Manager." Finest resolution 20 µs.
- **Units.** Positive `PrimeTime` count = ms; negative = negated µs (microsecond delays only in
  revised and extended). Max about 35 minutes in µs, about 1 day in ms. [Processes-56, -68]
- **Gestalt.** Selector `'tmgr'`: 1 original, 2 revised, 3 extended. If `Gestalt` errors, assume the
  original. [Processes-54, -73] **Observed in Snow on the 6.0.8 Classic image: `gestalt('tmgr')` = 3**
  [mt]. (That value is documented as "extended"; the same row records that `Microseconds` (A193)
  and `DTInstall` (A082) are *not* implemented, and `PrimeTime` in ms works.)
- **Task context.** Tasks run at interrupt time; do not call the Memory Manager; A5 may not be
  yours. With revised/extended managers A1 = the task record address on entry (set by `InsTime` /
  `InsXTime`); retrieve it first. [Processes-54, -60] Observed: Time Manager task runs at SR $2110
  (level 1) in Snow [mt].
- **`PrimeTime` semantics.** Task must be installed first by `InsTime` or `InsXTime`; a count of 0 runs
  "as soon as interrupts are enabled"; registers on entry A0 = task, D0 = delay; revised sets the high
  bit of `qType` as "active". `RmvTime` returns remaining time in `tmCount`. [Processes-68, -62]
- **Min interval.** Apple: "avoid specifying very small delay times as a way to execute a routine
  repeatedly ... On low-performance CPUs, little or no time may be left for other processing."
  [Processes-56] Observed [mt]: a task re-primed from itself every 1 ms managed **1585 runs in 2000 ms**
  (about 0.79 per ms) and slowed a busy loop by **28%**.
- **Drift.** `InsXTime` (extended manager) "can be used in place of `InsTime`" and is for drift-free
  periodic scheduling in the extended manager (the page does not describe the mechanism in detail;
  Processes-60/-73 only list it). Re-priming from inside the task accumulates execution latency,
  so wake times drift unless computed from a start time.

### What this means for MoofPort

- Polling at ms-scale is feasible but expensive: the measured 28% cost at 1 ms is for an *empty*
  task, so the plan's idle back-off and a 2-4 ms active window are the right shape. Decide by the
  Phase 4 CPU measure, not by guessing. Use ms (positive) values; µs buy nothing at our rates, and
  below ~1 ms the task overhead dominates.
- Because `Microseconds` and `DTInstall` are absent, microsecond timing for the profiling kit has to
  come from VIA timer reads or `TickCount`-averaged loops; do not call A193.
- A task that wants the poll to run at level 1 should run the SCSI transaction itself only after
  `SCSIGet` succeeds; otherwise it sets a "wanted" flag and returns. Alternative woken-by-interrupt
  options: VBL task (60 Hz, coarse, same interrupt level); the driver's own `drvrDelay` periodic
  `Control(accRun)` (the Device Manager calls a driver every N ticks when `dNeedTime` is set) runs
  at *not-interrupt* level and cheap, but its minimum period is 1 tick, so it is no better than VBL.
  The accRun statement is from general Device Manager behavior (Devices chapter); it is not
  re-verified for this document (see OPEN-10).

### OPEN

- **OPEN-9: real-Classic `gestalt('tmgr')` and the actual achievable `PrimeTime` periodicity**
  on 68000 hardware with BlueSCSI idle. Experiment: EnetBench: re-prime at 1, 2, 4 ms, count runs
  over 10 s, report % CPU via the busy-loop counter. The Snow figure came from a different timing model.
- **OPEN-10: `accRun` (`dNeedTime`, `drvrDelay`) as a polling source.** Documented in IM Devices
  (not fetched here); on System 6 its minimum period and interrupt state need a test.
- **OPEN-11: whether the Time Manager on 6.0.8 is the rev. 2 or 3 code path on the Classic** (Snow
  says 3; Apple documents rev. 2 for 6.0.3 and extended for 7.0). Experiment: the same as 9, also
  call `InsXTime` and see whether it traps.

---

## 5. The `.ENET` driver API (System 6)

### Commands and numbers

csCodes 245-253 (`EAddMulti`, `EDelMulti`, `EAttachPH`, `EDetachPH`, `EWrite`, `ERead`, `ERdCancel`,
`EGetInfo`, `ESetGeneral`) are observed in DaynaPORT's dispatch and by EnetSpike [mt]. No Apple
document gives the numbers (the 1987 spec also gives none). Names and semantics:

| Call | Semantics (source) |
|---|---|
| `EAttachPH` | `eProtType` + `ePointer` (handler, or NIL for the default handler). Protocol type 0 = IEEE 802.3. Result `LAPProtErr` (-94): "Protocol handler is already attached or node's protocol table is full". Call early in init. [IM-N Networking-271] |
| `EDetachPH` | `eProtType`. |
| `EWrite` | `ePointer` = write-data structure (WDS), a sequence of length/pointer pairs ended by length 0. First entry is the 14-byte header (6 dst, 6 unused, 2 type). Padding: "If the size of the packet you provide is less than 60 bytes, the driver adds pad bytes." Over 768 bytes requires `ESetGeneral` first. `eLenErr` -92 "Packet too large or first entry ... did not contain the full 14-byte header"; `excessCollsns` -95 hardware error. [IM-N -274; 1987 spec p.1] Source address: IM-N is silent for Ethernet; the 1987 spec "doesn't say the driver fills in the source address"; DaynaPORT 1.2.5 does write the node address at buffer+6..+11 [mt]. |
| `ERead` / `ERdCancel` | Only after `EAttachPH` with NIL handler. Buffer >= 1514 bytes for general packets (621 for EtherTalk). Several async `ERead`s can be pending, each with its own PB. `buf2SmallErr` -3101 truncation, `reqAborted` -1105 on cancel/detach. A packet with no pending `ERead` is discarded. Must not be called synchronously at interrupt time. [IM-N -275, -262] |
| `EGetInfo` | `ePointer` buffer >= 18 bytes (78 if SONIC). `eDataSize` = bytes returned. Layout: bytes 1-6 node MAC; 7-10 receive-queue overflow count; 11-14 transmit timeouts; 15-18 packets with incorrect address. SONIC cards return up to 78 bytes: bytes 19-78 are fifteen 4-byte counters. "For all cards that are not SONIC based, this field will contain a value of 18." [IM-N -278] The 1987 spec: first 6 bytes = MAC, others "none currently defined." Apple's own sample code uses a **78-byte** buffer and reads only the first 6 bytes (https://developer.apple.com/library/archive/samplecode/Get_Ethernet_Address/Listings/GetEAddr_c.html). |
| `ESetGeneral` | No arguments; switches from limited (768-byte tx buffer, so packets up to 621 bytes) to general mode (1514). Irreversible until reboot; returns `memFullErr` -108 if it cannot allocate. [IM-N -279; 1987 spec p.2: "may reset the hardware and could lose an incoming packet"] |
| `EAddMulti` / `EDelMulti` | `eMultiAddr` (6 bytes, in the PB). Per-address reference count in the driver: accept while count >= 1. `eMultiErr` -91 "Invalid address or table is full". Don't register the broadcast address. [IM-N -281] |

PB offsets are as measured in [mt]: `ioCompletion`@12 `ioResult`@16 `ioRefNum`@24 `csCode`@26
`eProtType`@28 `ePointer`@30 `eBuffSize`@34 `eDataSize`@36. (`eMultiAddr` overlays the same area from
@28; **not verified**, see OPEN-12.)

### Protocol handler

- "The CPU is in interrupt mode" on entry. A0/A1 are driver-private until after `ReadRest`; A2, D0,
  D2, D3 free; A3 = first byte after the 2-byte type field; A4 = `ReadPacket` (`ReadRest` = A4+2);
  D1 = bytes left after the header; A5 free until after `ReadRest`. Protocol type is at A3-2,
  source at A3-8, destination at A3-14. [IM-N Networking-262]
- `ReadPacket`: A3 = buffer, D3 = byte count (nonzero); on exit A3 past the data, D1 = bytes left,
  D3 = 0 on success, error = **Z flag cleared**; after an error do not call it or `ReadRest` again,
  just `RTS`. Don't request more than remain.
- `ReadRest`: A3 = buffer, D3 = buffer size (word, may be 0). On exit D3 = 0 exact, **< 0 packet too large
  for buffer, extra = -D3**, **> 0 spare buffer bytes**; A0-A2 and D2 preserved; D0/D1 changed. "You
  must terminate execution of your protocol handler with an RTS instruction whether or not `ReadRest`
  returns an error." Not calling `ReadRest` after the last `ReadPacket`: "the system will crash."
  After `ReadRest` only A0-A3/D0-D3 may be changed; no application globals.
  [IM-N -262; same rules in the 1987 spec pp.2-3, where D1 on entry "may be reduced to skip pad bytes".]
- Must have buffers allocated before the interrupt. Stack: the handler runs on whatever stack was
  current (the interrupted app's); depth OPEN [mt].

### Observed in DaynaPORT 1.2.5 [mt], not in any Apple document

D1 = frame length - 14 + 4 (counts the 4 CRC bytes); SR = $2600 in every handler call; handler can be
re-entered from inside `EWrite`; `EGetInfo` writes only the 6-byte MAC and leaves `eDataSize` = 0; a
synchronous `EWrite` hangs from a handler, a TM task, or with the interrupt mask > 0; async `EWrite`
always works.

### What this means for MoofPort

- `EGetInfo`: return 18 bytes (MAC + three counters the device already supplies as zeros) and set
  `eDataSize` = 18, always writing no more than `eBuffSize`. That is the documented non-SONIC
  contract. Clients that pass 78 bytes (Apple's sample) get 18 and are told so. Mirror DaynaPORT's
  smaller footprint only if a client demonstrably needs `eDataSize` = 0 (none known). Count real
  overflows/timeouts in the first three counters if cheap.
- `ESetGeneral`: no-op returning `noErr` (we always handle 1514).
- Provide `EAddMulti/EDelMulti` with a refcount table; map to firmware `0x0D` (device accepts 6 bytes);
  return -91 when the table is full. Table size is not specified by Apple. A small fixed table (8)
  is enough for AppleTalk (one multicast address) and nothing else on this setup.
- Protocol handlers: deliver exactly as documented, including the 4 extra bytes in D1 only to match
  Dayna (our handler contract with MacTCP is the one measured in [mt]; the Apple text says D1 = bytes
  following the header). **OPEN-13** below.
- Sync `EWrite` must not hang: implement non-blocking transmit as planned.

### OPEN

- **OPEN-12:** the `eMultiAddr` offset and csCode numbers for `EAddMulti`/`EDelMulti` (observed only by
  dispatch). Experiment: EnetSpike calls 245/246 against Dayna with a 6-byte address at @28 and
  checks the multicast filter on the wire (BlueSCSI log shows the 0x0D command).
- **OPEN-13: does MacTCP (or AppleTalk) rely on D1 including the 4 CRC bytes?** Documented: "number of
  bytes following the Ethernet header". Observed: +4. Experiment: SpyENET logs D1 vs frame length for
  ARP, ICMP, TCP, and tests handler behaviors with D1 = exact length.
- **OPEN-14:** runt frames and frames > 1514 from the device (not exercised in Snow).

---

## 6. MacTCP on Ethernet

### What is established

- **MacTCP uses `.ENET` directly, bypassing the LAP Manager.** NW540: "MacTCP accesses the .ENET driver
  directly, bypassing the LAP Manager." and closing it from the Network cdev while MacTCP still uses
  it "would likely" crash. Link choice is made in the MacTCP control panel (icons per link layer:
  LocalTalk, Ethernet, numbered per slot when several cards, e.g. "Ethernet (4)").
  [MacTCP Administrator's Guide 1989 pp. 41, 65-73,
  https://ftp.mirrorservice.org/sites/www.bitsavers.org/pdf/apple/mac/developer/Networking/MacTCP_Administrators_Guide_198901.pdf]
- **Driver names.** The old Apple sample (Get Ethernet Address) opens `".ENET"` with `OpenDriver`; on
  Mac II the slot is given to the Open call (1987 spec); Mac OS 8.5 adds a shim called `.ENET0` [Apple Q&A NW57:
  https://developer.apple.com/library/archive/qa/nw/nw57.html]. DaynaPORT registers `.ENET0`, and
  `OpenDriver(".ENET")` and `.ENET1` return -43 [mt]. So `.ENET0` as a name is a real
  convention, and not DaynaPORT-specific.
- **`adev`/`atlk`.** NW540: each non-LocalTalk AppleTalk connection type has its own `'adev'` file with
  an `'adev'` resource (handles all Network cdev interaction) and an `'atlk'` resource (the
  implementation); the Network cdev stores the selected connection in parameter RAM. This is the
  AppleTalk side only. The Dayna install includes none of these [mt], and MacTCP on this machine
  shows "Ethernet Built-In" without them. So **whatever makes MacTCP 2.1 offer Ethernet does not need
  ADEV/atlk for this driver** (inference from [mt] and the wiki install steps).
- **The BlueSCSI wiki recipe**: install MacTCP 2.1 (System Folder on System 6), install DaynaPORT 7.5.3
  drivers (Custom install, driver only), choose "Ethernet Built-In" in MacTCP, click More. Avoid AppleTalk
  v58 (a poster reports MacTCP Ping saying the drivers can't be located). 7.5.5 driver reported not
  working. (https://github.com/BlueSCSI/BlueSCSI-v2/wiki/WiFi-DaynaPORT; tinkerdifferent
  threads https://tinkerdifferent.com/goto/post?id=35026 and
  https://tinkerdifferent.com/threads/bluescsi-v2-daynaport-wifi.2857/page-5; Snow manual
  https://docs.snowemu.com/manual/network/ethernet, which tells the user to use MacTCP 2.0.6+).
- **"MacTCP 2.1".** The only public MacTCP 2.1 found is Glenn Anderson's unofficial patch of 2.0.6:
  "2.1 is just a user interface change to MacTCP to allow classless subnet mask configuration"
  (https://mactcp.net/mactcp.html). Whether the copy on the BlueSCSI "PicoW" disk is that patch is
  **not stated anywhere found**. The note that Apple's 2.0.6 "shows only LocalTalk" with this driver
  [mt] and that 2.1 shows "Ethernet Built-In" is observation, with no source explaining why; the
  patch description does not mention Ethernet detection.
- **MSS.** The MacTCP Programmer's Guide (1989): `UDPMTU` returns "the network MTU for local destinations,
  otherwise a value of 548"; the guide says IP guarantees reassembly of 576 bytes; TCP "negotiates
  packet size"; receive window minimum 4096, 8192+ recommended; `TCPStatus` reports "maximum segment
  size that can be sent". The guide never says how MSS is computed. A 576-byte limit (548 payload for
  UDP is 576-28; TCP MSS 536 is 576-40) is the classic default for **non-local** destinations
  (RFC 879/1122), so the observed 536 is consistent with "destination not on the local subnet". The
  document gives no way for the driver to influence MSS: the driver's only inputs are frame size
  limits and `EGetInfo`.

### What this means for MoofPort

- Treat the name `.ENET0` as the contract and verify MacTCP does not look for more: the likely
  mechanism is that MacTCP's control panel enumerates Ethernet drivers by name (`.ENET`, `.ENET0`, ...)
  or by slot, and we are only 1 of 1. SpyENET must log every `Open/Control/Status/GetInfo` and
  the `DCE` fields MacTCP reads (flags, `dCtlSlot`, name). If MacTCP reads `dCtlSlot` / slot-resource
  info, `DrvrInstall` of a pointer driver will not provide it; then ship as `DRVR 47`.
- MSS: assume 536 is off-subnet behavior. If the subnet-local MSS is bigger (up to 1460), the plan's
  packet rate assumption in §1.2 changes by 2.7x for local transfers; test against a local-subnet host.
  The driver cannot raise it; the real levers are frame rate and per-frame latency (our job).
- Copy provenance: record the MD5 and version resource of the "MacTCP 2.1" used in tests so results
  are reproducible.

### OPEN

- **OPEN-15 (key): how MacTCP 2.1 decides to show "Ethernet Built-In" and which name/unit it opens.**
  SpyENET log of the MacTCP control panel opening, plus a unit-table dump with and without the
  Dayna driver and with only our driver (OPEN-1). Compare 2.0.6 and 2.1 `cdev`/`drvr` code strings
  for `.ENET` (resource parse only, nothing committed).
- **OPEN-16: which `.ENET` calls MacTCP 2.1 makes, sync/async, from what context** (phase 0 item 2):
  SpyENET.
- **OPEN-17: MSS in the SYN's options for a local-subnet vs off-subnet peer, and TCP window sizes.**
  Experiment: `tcpdump`/Snow pcap on the Snow-side network while MacTCP connects to an in-subnet
  host and via gateway.
- **OPEN-18: provenance of BlueSCSI's "MacTCP 2.1"** (Glenn Anderson patch of 2.0.6, or another).
  Experiment: compare the file hash with the patch download at mactcp.net, or read the `vers` resource.

---

## 7. BlueSCSI Wi-Fi Desk Accessory: how it talks to the device

### What is established

- The DA (Joshua Stein, ISC license; https://jcs.org/bluescsi, source at https://amendhub.com/jcs/wifi_da
  `scsi.c`, `wi-fi.h`) works on System 6 and later (tested on 6 and 7; Font/DA Mover on 6).
- It uses the **old SCSI Manager directly, not `.ENET0`**; `scsi.c` has no reference to `.ENET0`.
  Sequence: `SCSIReset()` once before scanning; then for each ID 0-7 an INQUIRY (CDB `0x12`, first 5 bytes,
  then the reported length) and a match on vendor `"Dayna"` and product `"SCSI/Link"`, then
  `scsi_wifi_info()` as a confirmation. Each command: `SCSIGet`, `SCSISelect(id)`, `SCSICmd(cdb)`, one
  `SCSIRead`/`SCSIWrite` with a two-instruction TIB (`scNoInc` to a static buffer, `scStop`), then
  `SCSIComplete(&stat, &msg, 300)` (comment "1/60 ticks"). No LUN is used.
- The Wi-Fi command is CDB opcode `0x1C` with the subcommand in byte 1: 1 scan start, 2 scan complete,
  3 results (size in CDB[3..4]), 4 info, 5 join (write). [Toolbox Developer Docs wiki,
  https://github.com/BlueSCSI/BlueSCSI-v2/wiki/Toolbox-Developer-Docs; DA `scsi.c`; `[fw]`.] Up to 10
  networks per scan (`MAX_WIFI_NETWORKS`).
- The BlueSCSI Wi-Fi guide says to "use the WiFi DA to connect" (iPhone tethering or ini file). [BlueSCSI wiki]

### What this means for MoofPort

- **Coexistence is by the shared SCSI bus, not by the driver.** The DA runs when the user opens it,
  in the application context, and takes the bus via `SCSIGet`; MoofPort's polling must tolerate
  `scMgrBusyErr`/`scArbNBErr` (it already must). A DA call takes one synchronous SCSI transaction;
  both sides see a normal bus.
- **`SCSIReset()` is the hazard.** The DA resets the SCSI bus when scanning. A reset in the middle of
  our transaction aborts it, and may clear the device state: the firmware's *enable* command (0x0E) state
  and its RX queue ("enabling clears the receive queue" [fw]). MoofPort should check the bus-reset
  condition (error returns from `SCSIComplete`/`scCommErr`) and re-issue enable and re-register
  multicast after any unexplained error. Clearing the device state is cheap for us.
- The DA's INQUIRY loop reads the device's INQUIRY at every ID; MoofPort needs no such scan, but when
  `SCSIGet` for our configured ID fails persistently it should re-INQUIRY (with a short length, as the
  DA does) before giving up. MoofPort's own ID discovery (it cannot be hard-coded) can reuse the same
  INQUIRY match as the DA: vendor `Dayna`, product `SCSI/Link`.
- Do not hold the SCSI bus across calls: never issue `SCSIGet` and leave without `SCSIComplete`.
- INQUIRY lengths seen in the wild: the Dayna driver requests 17 bytes (SCSI2Pi issue #5); the DA
  probes with 5 and then re-asks with the reported additional length. Whether the firmware answers all
  lengths identically is not checked here (`[fw]` INQUIRY handling not reviewed for this document).

### OPEN

- **OPEN-19: Does the DA behave when `.ENET0` is polling at 1 ms?** Experiment: run the DA scan and
  join while a TCP download is running; look for aborted transactions and RX loss. Also verify what
  `SCSIReset` does to the device queue (BlueSCSI log).
- **OPEN-20:** The DA's source was read through an HTML view, not byte-for-byte; verify the `SCSIReset` and
  the 300 tick wait value against the actual `scsi.c` before relying on them (both are from a
  summary of the page).

---

## Decision summary (for the lead)

1. Unit-table install of `.ENET0` is documented and works by DV23 recipe; must be validated on 6.0.8
   (OPEN-1, OPEN-15) before committing to the INIT design. Fallback `DRVR 47` stays.
2. Blind transfers: Apple's own rule (first byte of each TIB instruction is polled; 16 µs max
   inter-byte latency) matches the plan's split design. Place TIB boundaries exactly at the 75 µs and
   300 µs pauses; use one TIB program per command (multiple `SCSIRead` calls per command are a documented
   bug). Polled fallback first.
3. `scMgrBusyErr` is the contract for "bus is in use by the File Manager". Defer and retry from the timer.
4. Time Manager: ms-rate polling costs about 28% CPU per 1 ms re-prime in Snow [mt]; `Microseconds`
   is unavailable. Back-off design is right; measure on hardware.
5. `.ENET` API: `EGetInfo` returns 18 bytes and sets `eDataSize` = 18; `ESetGeneral` no-op;
   multicast needs a refcount table; sync `EWrite` must be non-blocking.
6. MacTCP's MSS of 536 is probably the non-local default (576-40), not driver-controlled; test local
   subnet.
7. The Wi-Fi DA uses direct SCSI (vendor 0x1C), not `.ENET0`, and starts with `SCSIReset`; be
   robust against a reset and re-enable the device.

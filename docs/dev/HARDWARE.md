# Real-hardware sessions

Speed cannot be measured in Snow, so speed claims come from short sessions on a real Mac
Classic (PLAN.md section 7, decision 1). Everything else is tested in Snow. The staging script
has the same interface as MoofTCP's `scripts/stage-hw.sh`.

A session is one numbered kit (the names hw1, hw1b, hw2, ... and hwsend* in the docs): a spec
(Install, Try, Undo steps), the tools to run, and a checklist with blanks for the numbers. The
specs and the raw returns (filled checklists, `log.txt`, SpyENET text) are not in the repository;
the numbers that matter are in `BENCH.md`, in the column for that session. Reuse the step
wording of an earlier spec where the test is the same, so the numbers compare.

## Setup that stays fixed

- Mac Classic (68000, 7.83 MHz, 4 MB), System 6.0.8, MacTCP 2.1, DaynaPORT SCSI/Link 1.2.5
  from the 7.5.3 package (PLAN.md §1.1).
- BlueSCSI v2 (Pico W), firmware **v2026.08.17** (PLAN.md §7, decision 2). Same SD card, same
  Wi-Fi network, same router, same LAN host for all three sessions. Write down any change.
- A LAN machine runs `scripts/testnet.py` (ports 17007 echo, 17009, 17023, 17030 bulk send to
  the Mac, 17031 bulk receive from the Mac). The Mac reaches it over Wi-Fi via the BlueSCSI.
  Record the host's address and whether it is wired or Wi-Fi.
- MacTCP set to the same address, mask and router every session (record them).

## Procedure

1. **Write a spec** (developer): markdown with `## Install`, `## Try` (numbered actions with
   the expected result) and `## Undo`. Plain steps for the tester, no developer notes.
2. **Stage** (developer, host):

       scripts/stage-hw.sh --spec my-spec.md hw1 build-mac/<...>/SpyENET.bin ...

   Writes `dist/hw/hw1/` with each file as MacBinary (`.bin`) and BinHex (`.hqx`), a
   `README.txt` (ASCII, CR line endings) and `CHECKLIST.md` rendered from
   `scripts/hw-checklist-template.md`. Inputs must be MacBinary `.bin` files. Both formats are
   staged because it is not known which survives the BlueSCSI transfer with type, creator and
   resource fork intact; the checklist asks which one did. `scripts/stage-hw.sh --verify hw1`
   re-checks a staged session. `dist/` is gitignored.
3. **Copy** (tester): put the files in the BlueSCSI shared folder on the SD card using the
   BlueSCSI toolbox / shared-folder transfer, then pick them up on the Mac.
4. **Run** the checklist. Fill in the *Report back* section as you go (blanks are for numbers).
5. **Undo** at the end restores the machine. Nothing the session installs outlives it.
6. **Return**: send `CHECKLIST.md` with the blanks filled, the photos, and `log.txt` (below).

## Which files each session stages

hw1: `SpyENET`, `SpyDump`, `EnetBench`, `TcpBench`, `CpuMeter`, `HeapInfo`, `HeapInfoOpen`.
hw2 and hw3 add the MoofPort INIT and its preferences file, and drop SpyENET if it is no
longer needed. Keep HeapInfo and HeapInfoOpen in every session.

## BlueSCSI firmware version and log.txt

The firmware version decides which SCSI paths exist (multi-packet READ, batched WRITE need
v2026.08.17 or later, PLAN.md §7), so it goes in every report.

1. Power the Mac off. Take the SD card out and read it on a computer.
2. Copy `log.txt` from the card root to the report folder. Take it after the session's last
   test so it covers the whole run; if the card has been used since, say so.
3. Note the firmware version from the first lines of `log.txt` (expected `v2026.08.17`).
   OPEN: confirm the exact wording of that line on the first real log.
4. Record the card's `bluescsi.ini` settings that touch the network or SCSI speed (copy the
   file into the report folder). Do not edit them during a session.

## Safety and undo

- Work on a copy: back up the System Folder (or the whole boot volume image on the SD card)
  before hw1 and keep the backup until hw3 is done.
- SpyENET is an INIT in the System Folder. If the Mac does not boot, hold Shift at startup
  (extensions off), or boot from a spare image, and remove the INIT. Take a photo of any
  MacsBug or bomb message before restarting.
- MoofPort (hw2, hw3) is never installed over Dayna's driver file. It is a separate INIT, and
  Undo removes it. The DaynaPORT driver in the System file is not touched in any session.
- Do not run EnetBench while MacTCP is open (both use `.ENET0`). Restart before and after it.
- If a step hangs for more than a minute, note which step, photograph the screen, restart,
  and carry on with the next step. A hang is a result.
- Do not erase or rewrite the SD card in a session. Only add or remove the staged files.

## Memory check (the hard cap)

PLAN.md §2.4 caps MoofPort's resident memory at DaynaPORT's. Method follows MoofTCP's
`docs/dev/EMULATOR.md` "MacTCP memory baseline" (HeapInfo reads the system zone's `FreeMem`,
`MaxMem` and zone size; HeapInfoOpen shows FreeMem before and after opening a driver).

- **Occupied** = zone size minus FreeMem, in bytes, system heap. Take every figure on a fresh
  boot, run the tool twice and record the second run (the first can include one-time
  allocations).
- **Dayna footprint** = occupied with `.ENET0` open minus occupied with it not open, on boots
  with MacTCP installed but not opened. MacTCP must stay installed: without its `sysz` the
  system heap has about 14 KB free and the driver's open fails with -1 (Snow, 2026-10-31).
  `HeapInfoOpen` gives the open delta directly; the two plain `HeapInfo` runs confirm it.
- **MoofPort footprint** = (boot-time delta from its INIT) + (open delta), same method.
- **Rule:** MoofPort footprint, in bytes, must be less than or equal to Dayna's. Compare the
  two totals, not the code sizes. Also record the whole-stack figure (MacTCP active, driver
  opened by MacTCP) for both, as a cross-check against Snow's 15,952-byte MacTCP open cost.
- Remove SpyENET and SpyDump before any memory run; SpyENET sits in the path and adds RAM.

OPEN: removing Dayna completely is not possible without editing the System file, so "without
Dayna" means "driver present on disk but never opened". Confirm that is acceptable, or the
tester can try a spare System file with DRVR 47 removed (ResEdit; not part of the plan).

## Results flow

1. The tester returns the filled checklist, photos and `log.txt`.
2. Developer copies each number into `docs/dev/BENCH.md` in the column for that session
   (HW-Dayna for hw1, HW-MoofPort for hw2 and hw3), with the date, firmware version and the
   checklist file name in the "Runs" table.
3. Raw returns (checklist, `log.txt`, SpyDump text) are kept outside git.
4. Anything that surprised us goes in the "Notes" of the affected BENCH.md row, and decisions
   that follow (blind transfers go/no-go, poll defaults, the hard memory cap) are recorded in
   PLAN.md §7.
5. Speed claims in README or release notes may only quote BENCH.md hardware columns.

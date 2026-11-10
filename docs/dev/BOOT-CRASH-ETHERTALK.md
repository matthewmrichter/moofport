# Boot crash: "address error" after "Welcome to Macintosh" (EtherTalk + DaynaPORT)

Investigated on 2026-11-06 using the real-Classic disk image `HD00_512 System 6.0.8.hda` (BlueSCSI,
1.1 GB HFS volume "Mac Classic"). Status: **root cause found, reproduced in Snow, fixed on the disk; the patched driver (option 2) boots and works
on the real Classic with EtherTalk selected (2026-11-06).**

## Symptom

- The real Mac Classic (4 MB, System 6.0.8, BlueSCSI v2 with DaynaPORT Wi-Fi) shows the happy Mac and
  "Welcome to Macintosh", then "Sorry, a system error occurred. address error". No INIT icons appear.
- It was noticed after removing MoofTCP with the MoofTCP Installer (which put MacTCP back). The same
  day the hw1 kit (`MoofPort-hw1.sit.hqx`) had been copied to the disk with BlueSCSI Toolbox and expanded.
- The crash continued after MoofTCP was put back, so MoofTCP and MacTCP were not the cause.

## Root cause

The **Network control panel was set to EtherTalk** (the choice is stored in PRAM, not on the disk). At boot,
AppleTalk 58 (`INIT 18` in the System file) loads the EtherTalk Phase 2 `atlk` and opens `.ENET`, which reaches
DaynaPORT SCSI/Link 1.2.5 (`DRVR 47 ".ENET0"`, 12,512 bytes, in the System file). The driver's poll checks the
stack against `CurStackBase` (`$908`), which is -1 until the first application runs. It reads through that
value and takes an address error. Snow log for the reproduction:

    Address error: read = true, address = FFFFFFFF PC = 0003355A

MoofTCP found and documented the same driver bug when it opened `.ENET0` from its INIT
(`../mooftcp/docs/dev/MACTCP-INTERFACE.md`, "`.ENET0` is opened by the first `OpenDriver(".IPP")`": DRVR 47
offset `0x19F8`, `move.l (a0),d5` with A0 = `*(0x908)` = -1). MoofTCP avoids it by opening the driver late.
AppleTalk cannot avoid it.

This disk is **not the setup PLAN.md §1.1 assumes**. It has AppleTalk 58 (`INIT 18`, `atlk`, `ltlk`, `iopc`),
Apple's Ethernet Shell (`DRVR 127 ".ENET"` v127.0.5), the Network cdev and `EtherTalk Phase 2` (adev `et20`)
installed. BlueSCSI's recipe (Custom Install, SCSI/Link only) leaves all of these out.

## Reproduction in Snow

Snow starts with LocalTalk selected, so a plain boot of the disk does not crash. To reproduce:

1. Boot the disk (Mac Classic ROM, 4 MB, Ethernet at SCSI ID 3).
2. Apple menu > Control Panel > Network, choose **EtherTalk Alternative**, click OK.
   (The interface comes up fine while the Mac is running: `Interface enable: true`.)
3. Special > Restart. The bomb box "address error" appears before any INIT icon, exactly as on hardware.

PRAM survives a Special > Restart and Snow's toolbar reset, but not quitting Snow, **unless** the workspace has
`"pram_path"`. Snow then memory-maps that file as PRAM (log: `Persisting PRAM in ...`). The file must already
exist: a 256-byte file of zeros works, and the ROM initialises it. This is the reliable method, with no
in-session restart:

1. Workspace with `"pram_path": "/tmp/x.pram"`, zeroed 256-byte file. Boot, select EtherTalk as above, quit Snow.
   The file now holds the choice (bytes `$E0`-`$E3` = `FF FF 00 0A`; `$0A` is EtherTalk's `atlk` 10).
2. Copy that file before every run and cold-boot a fresh copy of the disk with it.

With the untouched image this gives the address error on every cold boot (2 of 2:
`Address error: read = true, address = FFFFFFFF PC = 0003355A`).

## Fix applied to the disk

`System Folder:EtherTalk Phase 2` was moved to `System Folder:Disabled Network`. At the next boot AppleTalk
shows "A driver for the selected AppleTalk connection could not be found. The built-in LocalTalk port will be
used instead." and the Mac reaches the Finder. Snow confirmed this with EtherTalk still selected in PRAM (3 of 3
cold boots with the EtherTalk PRAM file).
MacTCP 2.1 and MoofTCP keep working, because they open `.ENET0` after an application is running.
The loss is AppleTalk over Ethernet (AppleShare over Wi-Fi).

The fixed image is the original disk (MacTCP 2.1 restored by the uninstaller) plus that one move. The untouched
original is kept next to it as `HD00_512 System 6.0.8.hda.bak.2`.

## What was ruled out

- **Disk damage:** partition map, Apple_Driver43, boot blocks, MDB counters (free blocks against the bitmap,
  file and folder counts, next CNID) and a full catalog leaf walk are all consistent, and every resource
  fork parses. The only bad file is a truncated `System Folder:Registration Temp` (no type or creator,
  map past end of file). It is junk and harmless.
- **MacTCP 2.1 vs 2.0.6:** the restored "MacTCP 2.1" differs from 2.0.6 in about 288 bytes of `.ipp` and in the
  `ctrp` trap patch. It is the BlueSCSI-recommended build (`../mooftcp/docs/dev/GOLDEN-IMAGE.md`), not damage.
  Note that 2.0.6 offers only LocalTalk with this driver.
- **MoofTCP / its uninstaller:** running Remove in Snow and restarting gave a clean disk that boots. Putting
  MoofTCP back did not fix the real Mac.
- **AppleShare auto-mount:** `AppleShare Prep` lists no volumes to mount at startup.
- **Memory:** a red herring. See the next section.

## Pitfall: hfsutils changes the volume, and Snow then crashes differently

`hmount` sets the HFS "cleanly unmounted" bit (`drAtrb` `0x0100`) and increments `drWrCnt`, even if you only
read files. The real-Classic image had the bit clear (the Mac had crashed). With the bit **set**, Snow crashed on
every boot at "Welcome" (illegal-instruction loop at `$F518`/`$F48E`). That happened even with every INIT removed
and on 1, 2 and 4 MB. With it **clear**, 5 of 6 boots reached the Finder (one crashed the same way). This is a
separate Snow-side oddity, not understood. It made the early "2 MB hangs / INIT bisect" results meaningless.
Work on copies, and clear the bit again (byte at partition offset 1024+10, `&= 0xFE`) before boot tests that
need to match the real-Classic image.

A second failure looks like a hang at "Welcome", but it is a crash. The PC loops in the ROM's VIA level-1
interrupt dispatcher (`$402B2E`-`$402B54`) with SP at `$1B8`-`$1D8`, inside the low-memory globals. The stack is
gone, so the bomb box is never drawn, and Snow's trap history stays empty. It hit **every** in-session restart
and reset of this disk, and every cold boot (6 of 6) of a copy that hfsutils had edited (EtherTalk moved out
and back, `Disabled Network` folder made and removed). It happened with LocalTalk selected and with the stock
driver. The untouched image and the "fixed" image (also hfsutils-edited, but differently) do not show it. Cause
unknown. **Rule:** for boot experiments, change the image in place at known byte offsets (as for the patch below)
rather than through hfsutils, cold-boot it, and run an unpatched control the same way.

## Consequences for MoofPort

- **Requirement:** MoofPort's `.ENET0` must survive being opened at INIT time, before any application runs.
  That means no reliance on `CurStackBase`, `CurrentA5` or other application globals in Open or in the poll
  path. With that, EtherTalk can be selected, which DaynaPORT 1.2.5 cannot do. It belongs in the Phase 2
  compatibility matrix (PLAN.md §2.5 already lists "possibly EtherTalk later" as a client).
- **Hardware sessions:** record whether AppleTalk 58 / EtherTalk are installed and which AppleTalk connection
  is selected. The real Classic's disk differs from the fixed setup in `HARDWARE.md`.

## Option 2: patching DaynaPORT 1.2.5 (works in Snow and on hardware)

### What the driver does

DRVR 47 has two copies of the same stack guard, in the write path (`0x5fe`) and in the poll (`0x19e8`).
It calls `_StackSpace` (`A065`) into D6/D7, then computes `**CurStackBase - *(0x0BAE)`. If stack space is under
1024 bytes, or the computed value is lower, it takes a "stack too low" path. The poll re-primes its Time Manager
task (`_PrimeTime`, `A05A`), and after 40 deferrals it proceeds anyway (`a4@(832)` counter). The write path returns
`$40000000` for sync calls and -95 for async ones. The fault is the `move.l (a0),d5` (`0x19f8`) / `move.l (a0),d4`
(`0x60e`) through `CurStackBase` = `$FFFFFFFF`.

### The patches (32 bytes in place at each site, 34 bytes differ in total)

The frame slot `-8(a6)` that the original stores is never read again in either function, so it is dropped to make
room. Every branch target sets D0 before using it.

    v1 "skip": if CurStackBase is odd, treat the stack as fine
    0x5fe:  2078 0908  movea.l $908.w,a0     0x19e8: same, with d5/d7 and targets 0x1a1c / 0x1a08
            2008       move.l  a0,d0
            0800 0000  btst    #0,d0
            6656       bne.s   0x660         (v1)    v2 "defer": 6614 bne.s 0x61e (low-stack path)
            2810       move.l  (a0),d4
            226e fffc  movea.l -4(a6),a1
            9891       sub.l   (a1),d4
            0c86 0000 0400 6d04 b886 6c42    (original compare and branches, unchanged)

Original bytes at both sites: `598f 2eb8 0908 205f 2d48 fff8 226e fffc 2a10|2810 9a91|9891 0c87|0c86 0000 0400 6d04
ba87|b886 6c14|6c42`.

### Results in Snow

First round (in-session restarts, patch applied through hfsutils), superseded: v1 gave no address error, but
the boot ended in the "Welcome" crash described in the pitfall section (SP `$1D0`, PC `$402B4A`). v2 did the
same. The unpatched controls crashed **identically** (PC `$402B3E`-`$402B54`, SP `$1B8`-`$1D8`, same D4-D7 and
A3-A6), so that round said nothing about the patch.

Second round, the reliable method: cold boots with the EtherTalk PRAM file (see "Reproduction"). The control is
the untouched image. The patched image is the same image with v1 written **in place** at image offsets
`0x3f5c151` (write path) and `0x3f5d53b` (poll), inside the System file's second resource-fork extent (`cmp`:
exactly 34 bytes differ). The same patterns also occur at `0x3d9c356` / `0x3d9d740`, outside the System file;
those were left alone.

| Run | Image | Result |
|---|---|---|
| C1, C2 | untouched | address error, `PC = 0003355A`, before any INIT icon |
| P1, P2, P3 | v1 in place | Finder; no address error; no LocalTalk start; `Interface enable: true` at boot |

In P3 the Network cdev shows **EtherTalk Alternative** selected and "EtherTalk Version: 2.5" (unpatched
LocalTalk boots show "LocalTalk Version: 58.0"), so EtherTalk really loaded on the patched driver. PRAM still
reads `FF FF 00 0A` after boot. "No zones available" is expected on Snow's NAT link.

### Why v1 is low-risk after boot

The changed code only behaves differently while `CurStackBase` is odd, which is only before the first
application. After that, the original stack guard runs unchanged. At boot it treats the system stack as large
enough, which it is during INIT time.

### Status

v1 fixes the boot crash with EtherTalk selected. Snow: 3 of 3 cold boots, EtherTalk 2.5 loads. **Hardware
(2026-11-06):** the test image (`<work dir>/HD00_512 System 6.0.8 (EtherTalk patched driver TEST).hda` = the
untouched original + the 34 bytes, EtherTalk Phase 2 installed) boots on the real Classic and works, with
EtherTalk still selected in PRAM. Not yet measured: SCSI timing of the INIT-time polling and AppleTalk zones or
AppleShare over the Wi-Fi bridge in detail. For MoofPort, the requirement in "Consequences for MoofPort" stands:
its own `.ENET0` must survive an INIT-time open without a patch like this.

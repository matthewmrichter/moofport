# Changelog

## 1.1rc1 (release candidate, 2026-11-13)

MoofPort 1.1 works with MoofTCP 1.1 to make TCP faster. Under MacTCP it behaves as 1.0.

### What's new

- MoofPort 1.1 starts with 1.0's timing settings, so MacTCP users get 1.0's behaviour. (The `steadyack` setting
  was measured under MacTCP and was slower there: receive -10.2 KB/s, terminal burst +2.1 ms, echo +1.8 ms.)
- A `steadyack` setting: check the network every 16 ms, and once more 2 ms after each packet sent. MoofTCP 1.1
  switches MoofPort to it when it opens the driver. MoofTune has it too.
- A stable private interface for that switch (Status csCode 0x4D50 to detect MoofPort and its tune version,
  Control csCode 0x4D54 to set the timing), documented in `mac/driver/mp_drv.h`.
- MoofPortInstall can run unattended from a `MoofPortInstall.cfg` file (install or uninstall), for test
  harnesses. An unattended Uninstall refuses to remove the only Ethernet driver (no DaynaPORT driver kept).
  Its result file is named after the application. Run by hand it works as before.
- Test tools: MoofBatch (TCP send and receive timing) has the `rx4`, `chase1`, `steadyrx4`, `steadyack` and
  `keep` settings, a receive-buffer setting, a terminal test and a `stack:` line; AfpCopy and the other test
  tools end by themselves in unattended runs; MoofStat prints the frame log of the diagnostic driver build.
- Resident memory: 17,560 bytes, measured in the emulator (1.0: 17,528), within DaynaPORT's 16,554 + 1,024.

### Tested

- On a real Mac Classic (System 6.0.8, 4 MB, BlueSCSI v2 Wi-Fi), 2026-11-13, timed on the PC: MoofTCP 1.1 with
  MoofPort 1.1 against MoofTCP 1.0 with MoofPort 1.0, 10 paired blocks in one session. Medians: echo 53.6 ms ->
  about 44.5 ms, terminal burst (2,000 bytes) 153.6 -> about 131 ms, TCP receive 45.1 -> about 54.8 KB/s, queued
  send 38.0 -> about 41.0 KB/s, each better in all 10 pairs. One-at-a-time sends and AppleShare copies showed no
  significant difference. Measured over Wi-Fi; the figures vary with the network.
- MoofTCP 1.1's MoofDiag showed `steadyack` applied in all 10 blocks.
- Host unit tests with AddressSanitizer and UBSan; Snow runs of MoofBatch under MacTCP and under MoofTCP 1.1.

### Known limitations

- AppleShare volumes set to open at start-up can fail to mount when the AppleShare server is itself on Wi-Fi with
  Wi-Fi power saving on. AppleTalk's start-up lookups are multicast, and a power-saving Wi-Fi receiver misses most
  of them (about 1 in 7 boots with power saving on, 0 in 15 with it off). The stock DaynaPORT driver does the
  same. Turn power saving off on the server, use a wired server, or open the volume from the Chooser.
- The 17,560-byte resident figure is from the emulator; it is not yet confirmed on hardware.
- The limitations of 1.0 below still apply.

## 1.0 (first release, 2026-11-10)

First public release of MoofPort, an open-source `.ENET0` Ethernet driver for the BlueSCSI v2 Wi-Fi
(DaynaPORT emulation) on a Macintosh Classic with System 6.0.8. It replaces the driver of DaynaPORT
SCSI/Link 1.2.5.

### What's in it

- The `.ENET0` driver interface that MacTCP 2.1 and EtherTalk / AppleShare use, so both run on
  MoofPort at the same time, unchanged.
- A driver that sends each packet to the BlueSCSI at once, inside MacTCP's own call, and checks the
  device on an adaptive schedule: fast right after traffic, backing off when the network is quiet,
  and less while MacTCP is busy answering. A watchdog restarts the network check if it ever stops.
- The several-packets-per-command READ and WRITE of newer BlueSCSI firmware (v2026.01 and later),
  used when present; older firmware works one packet at a time.
- 17,528 bytes of resident memory, within 1,024 bytes of DaynaPORT's 16,554.
- A portable core in C99 (`core/`): no `malloc`, no global state, host-tested with sanitizers.
- **MoofPortInstall**: one-click Install, Update and Uninstall of the driver in the System file. It
  installs on a System with no DaynaPORT software at all; when DaynaPORT is installed, its driver is
  kept and Uninstall puts it back byte for byte.
- **MoofStat** (writes the driver's counters to a file) and **MoofTune** (changes the timing until
  the next restart; settings `default`, `noinline`, `steady`, `tick`, `txpolled`, `phase1`, `quiet`).
- Release files `MoofPort-1.0.dsk`, `MoofPort-1.0.sit` and `MoofPort-1.0.sit.hqx`, each with the
  three applications and a Read Me, plus `MoofPort-1.0.sha256`.

### Tested

- On a real Mac Classic (System 6.0.8, MacTCP 2.1, 4 MB, BlueSCSI v2 Wi-Fi), same session as
  DaynaPORT 1.2.5, timed on the PC: TCP receive 61.5 KB/s against 46.9 KB/s (+31%); sending, echo
  round trip and Wi-Fi frame loss level with DaynaPORT. AppleShare copies with an earlier build of
  the same design: 33 / 25 KB/s to / from a server, against DaynaPORT's 17 / 21.
- In the Snow emulator: install, update and uninstall with and without DaynaPORT, a clean System
  6.0.8 with Apple's Network Software Installer 1.4.5 (AppleTalk 58.1.4, EtherTalk 2.5.6), MacTCP,
  MoofStat and MoofTune.
- Host unit and end-to-end tests of the core against a fake device in three firmware personalities,
  with AddressSanitizer and UBSan.

### Known limitations

- The BlueSCSI's Wi-Fi link loses about 1-2% of full-size packets, varying with radio conditions;
  MacTCP resends after about a second. DaynaPORT is affected equally on the same hardware.
- Holding Shift at start-up does not skip MoofPort: it lives in the System file.
- Tested only on a Mac Classic with System 6.0.8.

## Pre-releases

- **1.0rc1** (2026-11-09): the 1.0 code with release-candidate version strings.
- **1.0b1** (2026-11-07, beta for hardware testing): first installer-based build. Faster receiving
  and AppleShare than DaynaPORT, sending about 20% slower, echo a few ms longer. Required DaynaPORT
  SCSI/Link to be installed.

# Changelog

From 2 on, MoofPort releases are numbered with whole numbers (1.0 was the first release, then 2, 3, ...); a
release candidate is `<n>rc<k>`. Git tags are `v<n>` (`v1.0` stays as it was).

## 2 (2026-11-18)

MoofPort 2 works with MoofTCP 1.1 to make TCP faster. Under MacTCP it behaves as 1.0.
It was developed as 1.1 (release candidate 1.1rc1, 2026-11-13) and is the first release under whole-number versions.

### What's new

- MoofPort 2 starts with 1.0's timing settings, so MacTCP users get 1.0's behaviour. (The `steadyack` setting
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
- Version numbers are whole numbers: the release files are `MoofPort-2.*`, and MoofPortInstall reports version 2.
  The version strings in MoofStat and MoofTune now say 2 as well (they had kept "1.0").
- Documentation: the install, update and uninstall steps are now in `INSTALL.md`; the README states what was
  tested (a Mac Classic with System 6.0.8; the Plus and SE should work but are untested; System 7 is not fully
  functional yet).

### Tested

- On a real Mac Classic (System 6.0.8, 4 MB, BlueSCSI v2 Wi-Fi), timed on the PC: MoofTCP 1.1 with MoofPort 2
  against 1.0's behaviour (the MoofTCP 1.0 engine, with MoofPort at 1.0's settings). The gains come from both
  together: MoofTCP 1.1's shorter call path and MoofPort's `steadyack` timing. 2026-11-14, 10 pairs in one
  session, the AppleShare server's Wi-Fi power saving off. Medians, each better in all 10 pairs: echo 53.6 -> 41.4 ms,
  terminal burst (2,000 bytes) 153.6 -> 124.6 ms, TCP receive 45.3 -> 55.6 KB/s. Not significant that day: queued
  send (36.7 -> 41.0 KB/s), one-at-a-time sends (25.2 -> 23.9 KB/s) and AppleShare copies (too few pairs); the
  network was noisy. The session the night before gave the same direction. Measured over Wi-Fi; the figures vary
  with the network.
- MoofTCP 1.1's MoofDiag showed `steadyack` applied in every MoofPort block.
- Host unit tests with AddressSanitizer and UBSan; Snow runs of MoofBatch under MacTCP and under MoofTCP 1.1.
- Release files: `release.sh 2` and `release-verify.sh --self-test 2` pass (every format read back with
  independent tools).

### Known limitations

- AppleShare volumes set to open at start-up can fail to mount. At start-up AppleShare finds the server with
  AppleTalk multicast lookups (4 tries in about 2 seconds, no retry), and on Wi-Fi these are lost far more often
  than normal traffic. Measured on the test setup: with the server's Wi-Fi power saving on, about 1 in 7 to 1 in 3
  boots failed; with it off, 0 of 15 at night but up to 1 in 3 at busy times of day. The stock MacTCP + DaynaPORT
  driver fails the same way, and in an interleaved test MacTCP + MoofPort failed 4 of 10 boots and MoofTCP 1.1 +
  MoofPort 1 of 10. It is not caused by MoofPort or MoofTCP. Workaround: open the volume from the Chooser; to make
  it rarer, reduce multicast loss (a wired connection for the server, the server's Wi-Fi power saving off).
- The 17,560-byte resident figure is from the emulator; it is not yet confirmed on hardware.
- The limitations of 1.0 below still apply.

### Going back to 1.0

Run the 1.0 MoofPortInstall, click Update (it installs its own driver whatever version is installed), and restart.

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

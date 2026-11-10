# Changelog

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

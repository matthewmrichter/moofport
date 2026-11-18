# MoofPort

**An open-source Ethernet driver for the BlueSCSI v2 Wi-Fi on a Mac Classic running System 6.0.8.**

![MoofPort 1.0 folder: MoofPortInstall, MoofStat, MoofTune and Read Me](docs/images/hero.png)

_The MoofPort 1.0 folder, as it comes out of the archive._

## What is it?

MoofPort is an Ethernet driver for the **BlueSCSI v2**'s Wi-Fi (DaynaPORT emulation) on a **68000 Macintosh** (tested on a Classic) with **System 6**. It works with **MacTCP 2.1** or [MoofTCP](https://github.com/matthewmrichter/mooftcp/).

It is a drop-in replacement for the `.ENET0` driver from **DaynaPORT SCSI/Link 1.2.5**. You run an installer, restart, and your Mac is on Wi-Fi again, with the same MacTCP settings. If you ever want to go back, one click on **Uninstall** puts DaynaPORT's driver back exactly as it was.

## Why MoofPort?

Why do anything on a 35+ year old computer? :)

For years the vintage DaynaPORT SCSI/Link driver has been the way to get a vintage Macintosh onto Wi-Fi with a BlueSCSI. It did that job well, and everyone who has a Mac Classic on a network owes it thanks. BlueSCSI's Wi-Fi emulation speaks DaynaPORT's protocol precisely so that this driver works.

But that driver is a closed piece of software that is long out of support, and it is not going to change. That has a cost:

- **Performance.** Nobody can tune a driver they cannot change. MoofPort receives data about **31%** faster than DaynaPORT on the same Mac, and AppleShare downloads are about **25%** faster, with uploads also ahead (numbers below).
- **Reliability.** When something goes wrong, nobody can look inside a closed driver to find out why. MoofPort counts what it does, and MoofStat shows you the counters.
- **Future maintenance.** New BlueSCSI firmware adds things (several packets per SCSI command, for one). A driver nobody can update cannot use them. MoofPort can.
- **Availability.** You should not need a long-dead, unsupported plug-in installed on your System just to get Ethernet. MoofPort installs on a clean System with no DaynaPORT software at all.

MoofPort keeps what worked: the same `.ENET0` interface, so MacTCP and AppleTalk cannot tell the difference, and the same small memory footprint (within **1 KB** of DaynaPORT's). It adds source code you can read, tests, and somebody to fix it.

## Features

- Drop-in `.ENET0` driver for MacTCP 2.1 and EtherTalk / AppleShare.
- Faster receiving than DaynaPORT 1.2.5, and level on sending and latency (see Performance).
- Installs on a System with no DaynaPORT software. If DaynaPORT is installed, it is kept for Uninstall.
- One-click Install, Update and Uninstall (MoofPortInstall). Uninstall restores DaynaPORT's driver byte for byte.
- Small: about 17,500 bytes resident, within 1 KB of DaynaPORT's 16,554.
- Uses newer BlueSCSI firmware's several-packets-per-command READ and WRITE when it is there, and works with older firmware one packet at a time.
- Open source (MIT), with host unit tests and emulator tests.

## Requirements

- A Macintosh Classic with System 6.0.8 (tested). The Plus and SE use the same 68000 and SCSI chip and should work, but are untested. System 7 is not fully functional yet.
- A BlueSCSI v2 with the Wi-Fi DaynaPORT feature. The BlueSCSI must connect to your Wi-Fi network.
- BlueSCSI firmware v2026.01 or later is recommended. This firmware can send several packets in one SCSI command. Older firmware also operates, with one packet in each command.
- MacTCP 2.1, AppleTalk with EtherTalk, or both.
- DaynaPORT software is not necessary. If DaynaPORT SCSI/Link is installed, MoofPort replaces its driver in the System file. MoofPort keeps the DaynaPORT driver for Uninstall.
- System memory: MoofPort uses 17,560 bytes (measured in the emulator). The DaynaPORT driver uses 16,554 bytes.

## Installing

Download `MoofPort-2.sit.hqx` from the Releases page, expand it on the Mac, open **MoofPortInstall**, click **Install**, and restart. [INSTALL.md](INSTALL.md) has the full steps: which file to download, getting it onto the Mac, installing, updating, uninstalling, and setting up EtherTalk without the DaynaPORT disks.

## Performance

Measured on a real Mac Classic (8 MHz 68000, 4 MB, System 6.0.8, MacTCP 2.1), 2026-11-10, in one session. DaynaPORT 1.2.5 and MoofPort 1.0 alternated in blocks (MoofPort, DaynaPORT, MoofPort, DaynaPORT), with the drivers switched unattended, so both saw the same Wi-Fi conditions. Times are measured on the PC, not by the Mac's clock; figures are medians. How it was measured is described in [TESTING_METHODOLOGY.md](TESTING_METHODOLOGY.md).

|                                                   | MoofPort 1.0    | DaynaPORT 1.2.5 |                                                                                       |
| ------------------------------------------------- | --------------- | --------------- | ------------------------------------------------------------------------------------- |
| TCP receive, 1 MB                                 | 61.5 KB/s       | 46.9 KB/s       | +31% faster                                                                           |
| TCP send, one 1,400-byte send at a time           | 26.5 KB/s       | 23.0 KB/s       | level: both are set by occasional Wi-Fi loss stalls in that session                   |
| TCP send, 4 sends queued, runs with no loss stall | 45.7 KB/s       | 46.1 KB/s       | level                                                                                 |
| Echo round trip                                   | 41.6 ms         | 40.6 ms         | level                                                                                 |
| Wi-Fi frames lost, all frames the Mac sent        | 11.3 per 1,000  | 13.9 per 1,000  | same or lower                                                                         |
| Wi-Fi frames lost, full-size frames               | 16.6 per 1,000  | 21.6 per 1,000  |                                                                                       |
| AppleShare copy, 336 KB, to / from the server     | 5.0 / 21.6 KB/s | 4.0 / 17.4 KB/s | downloads +24%; uploads +27% but within run-to-run noise (Wi-Fi retries that session) |
| Resident memory                                   | 17,528 bytes    | 16,554 bytes    | within 1,024 bytes                                                                    |

- **When no packet is lost, a single stream of sends runs at about 50 KB/s for both drivers.** The difference you see in everyday sending is the Wi-Fi link's occasional lost packet, not the driver.
- **AppleShare copy** was measured in a second session on the same Mac (2026-11-10), with the drivers alternating in blocks and all 16 copies checked byte for byte. Wi-Fi retries slowed the uploads for both drivers in that session. Without the retry pauses, MoofPort copied at 32.6 / 25.5 KB/s and DaynaPORT at 28.5 / 20.0 KB/s.
- **Memory:** MoofPort 2 uses 17,560 bytes (1.0: 17,528), measured with `HeapInfoOpen` in the emulator; the hardware figure for 2 is not confirmed yet. DaynaPORT's 16,554 was measured on the hardware.
- **MoofPort 2** starts with the same settings as 1.0, so these MacTCP figures are 1.0's. For MoofPort 2 with MoofTCP 1.1, see [CHANGELOG.md](CHANGELOG.md).

Receiving and file sharing are faster; sending and latency are level with DaynaPORT.

## Known issues

- **Occasional Wi-Fi packet loss:** now and then a packet is lost on the BlueSCSI's Wi-Fi link (about 1-2% of full-size packets in tests, varying with radio conditions). MacTCP sends it again after about a second, so a large transfer can pause briefly. DaynaPORT is affected equally on the same hardware.
- **If you hold Shift at start-up, MoofPort stays active.** MoofPort is in the System file, not in an extension.
- **AppleShare volumes set to open at start-up may not mount.** At start-up AppleShare finds the server with AppleTalk multicast lookups (4 tries in about 2 seconds, no retry), and on Wi-Fi these are lost far more often than normal traffic. Measured on the test setup: with the server's Wi-Fi power saving on, about 1 in 7 to 1 in 3 boots failed; with it off, 0 of 15 at night but up to 1 in 3 at busy times of day. The stock MacTCP + DaynaPORT driver fails the same way, and in an interleaved test MacTCP + MoofPort failed 4 of 10 boots and MoofTCP 1.1 + MoofPort 1 of 10. It is not caused by MoofPort or MoofTCP. Workaround: open the volume from the Chooser; to make it rarer, reduce multicast loss (a wired connection for the server, the server's Wi-Fi power saving off).

## Testing methodology

See [TESTING_METHODOLOGY.md](TESTING_METHODOLOGY.md).

## Tools

MoofStat and MoofTune are described in [TOOLS.md](TOOLS.md).

## Building from source

The build uses Retro68 in Docker, and the portable core has host tests:

    scripts/build-mac.sh MoofPortInstall     # also MoofTune, MoofStat
    cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
    scripts/release.sh --force 2             # dist/MoofPort-2.{dsk,sit,sit.hqx,sha256}, verified

`release.sh` also needs hfsutils, macutils, unar, gcc and Python 3.

`CLAUDE.md` lists all the commands. The release files (`.dsk`, `.sit`, `.sit.hqx`) are built by `scripts/release.sh`; `docs/dev/RELEASE.md` describes them.

## FAQ

**Do I need DaynaPORT installed?** No. MoofPort installs on a System with no DaynaPORT software. If DaynaPORT is installed, MoofPortInstall keeps its driver for Uninstall.

**Do I need the DaynaPORT disks for EtherTalk and AppleShare?** No. MoofPort supplies the `.ENET0` driver that EtherTalk uses. Apple's Network Software Installer 1.4.5 is one source of AppleTalk and EtherTalk. Use **Customize**, not Easy Install. See [EtherTalk without DaynaPORT disks](INSTALL.md#ethertalk-without-daynaport-disks) for the procedure and for what was tested.

**How do I go back to DaynaPORT?** Run MoofPortInstall, click **Uninstall**, and restart. See [Uninstalling](INSTALL.md#uninstalling).

**Does AppleShare work?** Yes, with EtherTalk, together with MacTCP. See [EtherTalk without DaynaPORT disks](INSTALL.md#ethertalk-without-daynaport-disks) for where to get EtherTalk.

**Which BlueSCSI firmware do I need?** Firmware v2026.01 or later is recommended. This firmware can send several packets in one SCSI command. Older firmware also operates, with one packet in each command.

**Which file do I download?** Use `MoofPort-2.sit.hqx` if you are not sure. See [Download](INSTALL.md#download).

**The Mac stops or shows a bomb at start-up.**

1. Shut down the Mac.
2. Restore the backup copy of the hard disk image ([Installing](INSTALL.md#installing), steps 1 to 4).

**The Mac starts, but the network does not operate correctly.**

1. Record the counters with MoofStat (see [TOOLS.md](TOOLS.md)). Keep the file that MoofStat writes.
2. Uninstall MoofPort (see [Uninstalling](INSTALL.md#uninstalling)).

**The Mac starts without the BlueSCSI.** The Mac starts normally. MacTCP and AppleTalk then show that the driver is not available.

**Why is a large transfer sometimes slow?** This is the occasional Wi-Fi packet loss (see Known issues). The DaynaPORT driver has the same loss on the same hardware.

## TODO

- More bake-in testing on real hardware.
- Sending with several packets queued: find the occasional slow turnaround. The typical gap is shorter than DaynaPORT's, but the average is only level.
- Echo latency: try the `steady` setting's handling of echo-sized replies, which matched DaynaPORT.
- Direct access to the Classic's NCR 5380 SCSI chip, bypassing the SCSI Manager (high risk if the boot disk shares the bus, as typical on BlueSCSI).
- Wi-Fi frame loss: find which radio hop loses frames with a 2.4 GHz monitor-mode capture. Needs different wifi hardware.
- MoofStat: print the driver version.
- Review the BlueSCSI codebase for chances of tighter integration.
- Publish the unattended test harness (driver switching, testnet stall logging) into the main branch. (Stay tuned for _MoofAgent_)

## See Also

- [MoofTCP](https://github.com/matthewmrichter/mooftcp/) - A modern, memory-lean replacement for MacTCP on 68k Macs running System 6

## Credits and license

- [Retro68](https://github.com/autc04/Retro68) for the 68k cross-compiler.
- [Snow](https://github.com/twvd/snow) for the Mac emulator the tests run in.
- [BlueSCSI](https://bluescsi.com) and DaynaPORT for the network hardware this was built around.
- [Claude Code](https://claude.ai) was used to develop this project, with human planning and testing.

MoofPort is not affiliated with Apple, Dayna Communications or BlueSCSI. No Apple or DaynaPORT code is included.

MoofPort is MIT licensed; see `LICENSE`.

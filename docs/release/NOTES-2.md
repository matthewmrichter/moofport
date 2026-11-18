# MoofPort 2

MoofPort 2 works with [MoofTCP](https://github.com/matthewmrichter/mooftcp/) 1.1 to make TCP faster on a Mac Classic with a BlueSCSI v2 Wi-Fi. Under MacTCP it behaves as 1.0 did.

MoofPort now numbers its releases with whole numbers: 1.0 was the first release, 2 is the second.

## Download

| File | What it is |
|---|---|
| `MoofPort-2.sit.hqx` | MoofPortInstall, MoofStat, MoofTune and Read Me in a BinHex-encoded StuffIt archive. If you are not sure, use this file: it is text, so a text transfer cannot damage it. |
| `MoofPort-2.sit` | the same four files in a StuffIt archive, for transfers that keep files byte for byte |
| `MoofPort-2.dsk` | the same four files on an 800K disk image (floppy emulator, 800K floppy or Mac emulator) |
| `MoofPort-2.sha256` | checksums of the files above (`sha256sum -c MoofPort-2.sha256`) |

Open **MoofPortInstall**, click **Install** (or **Update** if an older MoofPort is installed), and restart. Uninstall puts the DaynaPORT driver back exactly as it was.

## Highlights

MoofPort 2 starts with 1.0's timing settings, so under MacTCP it works as 1.0 did. MoofTCP 1.1 switches MoofPort to its `steadyack` setting when it opens the driver. Measured on a real Mac Classic (System 6.0.8, 4 MB, BlueSCSI v2 Wi-Fi), timed on the PC: MoofTCP 1.1 with MoofPort 2 against 1.0's behaviour (the MoofTCP 1.0 engine, with MoofPort at 1.0's settings). The gains come from both together: MoofTCP 1.1's shorter call path and MoofPort's `steadyack` timing. Medians:

|                             | 1.0       | 2         |
| --------------------------- | --------- | --------- |
| Echo round trip, 1 byte     | 53.6 ms   | 41.4 ms   |
| Terminal burst, 2,000 bytes | 153.6 ms  | 124.6 ms  |
| TCP receive, 1 MB           | 45.3 KB/s | 55.6 KB/s |

Measured 2026-11-14 in one session of 10 pairs, with the AppleShare server's Wi-Fi power saving off. These three were better in all 10 pairs (the session the night before gave the same direction). Not significant in this session: TCP send with 4 sends queued (36.7 -> 41.0 KB/s), one-at-a-time sends (25.2 -> 23.9 KB/s) and AppleShare copies (too few pairs with the volume mounted). The network was noisy that day: stock MacTCP's one-at-a-time send ranged from 11 to 40 KB/s. The figures were measured over Wi-Fi and vary with the network.

Also new: a stable private interface for that switch (`mac/driver/mp_drv.h`), an unattended mode of MoofPortInstall for test harnesses, and new MoofBatch settings. [CHANGELOG.md](https://github.com/matthewmrichter/moofport/blob/main/CHANGELOG.md) has the full list.

To go back to 1.0: run the 1.0 MoofPortInstall, click **Update** (it installs its own driver whatever version is installed), and restart.

## Known limitations

- MoofPort 2 uses 17,560 bytes of system memory, measured in the emulator (1.0: 17,528); the hardware figure is not confirmed yet.
- AppleShare volumes set to open at start-up can fail to mount on Wi-Fi. The stock MacTCP + DaynaPORT driver fails the same way; open the volume from the Chooser. See the README's Known issues.
- Tested on a Mac Classic with System 6.0.8. The Plus and SE should work but are untested; System 7 is not fully functional yet.

See the [README](https://github.com/matthewmrichter/moofport#readme) and the [CHANGELOG](https://github.com/matthewmrichter/moofport/blob/main/CHANGELOG.md).

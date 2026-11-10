MoofPort 1.0
============

MoofPort is an Ethernet driver (.ENET0) for the Wi-Fi of the
BlueSCSI v2 (DaynaPORT emulation) on a Macintosh Classic with
System 6.0.8. It replaces the driver of DaynaPORT SCSI/Link
1.2.5. MacTCP 2.1 and EtherTalk / AppleShare can use MoofPort
at the same time, as they use the DaynaPORT driver.

MoofPort keeps your MacTCP and AppleTalk settings. Uninstall
puts the DaynaPORT driver back exactly as it was.

This folder contains these files:

  - MoofPortInstall: installs, updates and uninstalls MoofPort.
  - MoofStat: writes the MoofPort counters to a file.
  - MoofTune: changes the MoofPort timing until the next
    restart.
  - Read Me: this text.


REQUIREMENTS

  - A Macintosh Classic, Plus or SE (a 68000 Mac with the same
    SCSI chip) with System 6.0.8.
  - A BlueSCSI v2 with the Wi-Fi DaynaPORT feature. The
    BlueSCSI must connect to your Wi-Fi network.
  - BlueSCSI firmware v2026.01 or later is recommended. This
    firmware can send several packets in one SCSI command.
    Older firmware also operates, with one packet in each
    command.
  - MacTCP 2.1, AppleTalk with EtherTalk, or both.
  - DaynaPORT software is not necessary. If DaynaPORT
    SCSI/Link is installed, MoofPort replaces its driver in the
    System file. MoofPort keeps the DaynaPORT driver for
    Uninstall.
  - System memory: MoofPort uses 17,528 bytes. The DaynaPORT
    driver uses 16,554 bytes.


ETHERTALK WITHOUT DAYNAPORT DISKS

MoofPort supplies the .ENET0 driver that EtherTalk uses.
EtherTalk does not need the DaynaPORT disks. You must have
Apple's AppleTalk Phase 2 with EtherTalk. The System 6.0.8 disks
do not include it.

To install AppleTalk and EtherTalk from Apple's Network Software
Installer 1.4.5:

  1. Open the Network Software Installer 1.4.5.
  2. Click Customize.
  3. Select "AppleTalk v58.1.4 for System 6".
  4. Select one "EtherTalk for ..." item, for example "Ethernet
     NB Card".
  5. Click Install.
  6. Restart the Mac.
  7. Select EtherTalk in the Network control panel.

NOTE: Easy Install installs AppleTalk without EtherTalk. Use
Customize.

NOTE: The DaynaPORT disk is a different source. Click Custom
Install and select "AppleTalk v58.0.0 for System 6".

This setup was tested in the Snow emulator on a clean System
6.0.8 with no DaynaPORT software (AppleTalk 58.1.4, EtherTalk
2.5.6). AppleTalk opened MoofPort at start-up, and MacTCP
operated. An AppleShare volume was not tested on this setup.
The AppleShare copies under PERFORMANCE used the AppleTalk and
EtherTalk from the DaynaPORT disk.


INSTALL

CAUTION: Make a backup copy of the hard disk image before you
install MoofPort. If the Mac does not start after the
installation, the backup is the sure way back.

   1. Shut down the Mac.
   2. Remove the SD card from the BlueSCSI.
   3. On a different computer, make a copy of the hard disk
      image file on the SD card.
   4. Put the SD card back into the BlueSCSI.
   5. Start the Mac.
   6. Open MoofPortInstall.
   7. Type a name for this run, for example the name of the
      Mac.
   8. Click OK.
      NOTE: MoofPortInstall puts this name in the name of its
      result file, "Installer-<name>.out".
   9. Click Install.
      NOTE: Cancel is the default button. MoofPortInstall
      changes nothing until you click Install.
  10. When MoofPortInstall shows "MoofPort is installed",
      click OK.
  11. Restart the Mac.

MoofPortInstall writes the file "Installer-<name>.out" next to
itself. This file records the changes.

For MacTCP, select the Ethernet icon in the MacTCP control
panel. MoofPort shows as "Ethernet Built-In". If you use
EtherTalk, keep EtherTalk selected in the Network control panel.


UPDATE

Use this procedure if MoofPort 1.0b1 or a different older
MoofPort is installed. MoofPortInstall then shows Update.

  1. Open MoofPortInstall.
  2. Type a name for this run.
  3. Click OK.
  4. Click Update.
  5. When MoofPortInstall shows the result, click OK.
  6. Restart the Mac.


UNINSTALL

  1. Open MoofPortInstall.
  2. Type a name for this run.
  3. Click OK.
  4. Click Uninstall.
  5. When MoofPortInstall shows the result, click OK.
  6. Restart the Mac.

MoofPortInstall puts the DaynaPORT driver back exactly as it was
before the installation.

CAUTION: If you installed MoofPort on a System without
DaynaPORT, MoofPortInstall has no driver to put back. After
Uninstall, the Mac has no Ethernet driver.


TOOLS

MoofStat writes the MoofPort counters (packets, errors,
settings) to the file "MoofStat-<name>.out", next to MoofStat.
If you think that MoofPort has a problem, record the counters
and keep this file:

  1. Open MoofStat.
  2. Type a name for this run.
  3. Click OK.
  4. When MoofStat shows "MoofStat finished", click OK.

MoofTune changes the MoofPort timing until the next restart.

  1. Open MoofTune.
  2. Type the name of a setting from the list below as the run
     name.
  3. Click OK.

  default   The MoofPort 1.0 settings (the settings after a
            restart).
  noinline  Send each packet in the next network check, not at
            once (the 1.0b1 method). Fewer lost packets in
            some tests, slower one-at-a-time sends.
  steady    Check the network every 16 ms, one packet at a
            time, as DaynaPORT does. Receive is approximately
            30% slower.
  tick      Check the network once per tick, always (0.3d3).
  txpolled  Handshake every byte of each packet that goes to
            the BlueSCSI. Slower, and no fewer lost packets.
  phase1    The behavior of the first version, for comparison.
  quiet     Check the network only every 30 s (for
            diagnostics).


PERFORMANCE

Measured on a real Mac Classic (8 MHz 68000, 4 MB, System 6.0.8,
MacTCP 2.1) on 2026-11-10, in one session. DaynaPORT 1.2.5 and
MoofPort 1.0 alternated in blocks, so both had the same Wi-Fi
conditions. The PC measured the times. The figures are medians.

                          MoofPort 1.0  DaynaPORT 1.2.5
  TCP receive, 1 MB       61.5 KB/s     46.9 KB/s  (+31%)
  TCP send, one 1,400-    26.5 KB/s     23.0 KB/s  (level)
    byte send at a time
  TCP send, 4 sends       45.7 KB/s     46.1 KB/s  (level)
    queued, no loss stall
  Echo round trip         41.6 ms       40.6 ms    (level)
  Wi-Fi frames lost,      11.3 / 1,000  13.9 / 1,000
    all frames sent
  Wi-Fi frames lost,      16.6 / 1,000  21.6 / 1,000
    full-size frames
  Resident memory         17,528 bytes  16,554 bytes

The one-at-a-time send figures are both set by occasional Wi-Fi
loss stalls in that session. When no packet is lost, a single
stream of sends runs at about 50 KB/s for both drivers.

AppleShare copy, 336 KB to / from a server (8 runs each, same
session, all copies byte-exact): MoofPort 5.0 / 21.6 KB/s,
DaynaPORT 4.0 / 17.4 KB/s. Downloads are 24% faster. Uploads
are 27% faster, but within the run-to-run noise: Wi-Fi retries
slowed the uploads for both drivers in that session.
Without the retry pauses: MoofPort 32.6 / 25.5 KB/s, DaynaPORT
28.5 / 20.0 KB/s.


KNOWN ISSUES

  - Occasional Wi-Fi packet loss: sometimes the Wi-Fi link of
    the BlueSCSI loses a packet (about 1-2% of full-size
    packets in tests). MacTCP sends it again after about one
    second, so a large transfer can stop for a short time. The
    DaynaPORT driver has the same loss on the same hardware.
  - If you hold Shift at start-up, MoofPort stays active.
    MoofPort is in the System file, not in an extension.


TROUBLESHOOTING

The Mac stops or shows a bomb at start-up:

  1. Shut down the Mac.
  2. Restore the backup copy of the hard disk image (INSTALL,
     steps 1 to 4).

The Mac starts, but the network does not operate correctly:

  1. Record the counters with MoofStat (TOOLS). Keep the file
     that MoofStat writes.
  2. Uninstall MoofPort (UNINSTALL).

A large transfer is sometimes slow:

  This is the occasional Wi-Fi packet loss (KNOWN ISSUES). The
  DaynaPORT driver has the same loss.

EtherTalk without the DaynaPORT disks:

  The DaynaPORT disks are not necessary. Use Apple's Network
  Software Installer 1.4.5 with Customize (ETHERTALK WITHOUT
  DAYNAPORT DISKS).

The Mac starts without the BlueSCSI:

  The Mac starts normally. MacTCP and AppleTalk then show that
  the driver is not available.


ABOUT

MoofPort is not affiliated with Apple, Dayna Communications or
BlueSCSI. It contains no Apple or DaynaPORT code. MoofPort is
MIT licensed. The source code, with the full license, is at
github.com/matthewmrichter/moofport.

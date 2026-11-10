# Phase 1: the minimum working driver (Snow)

Status 2026-11-07: done in Snow. Not run on hardware. PLAN section 4 has the phase definition.

## What it is

An INIT-type file (`build-mac/MoofPort.bin`, goes in the System Folder) that installs a `.ENET0`
driver for the BlueSCSI DaynaPORT emulation. MacTCP 2.1 uses it in place of DaynaPORT SCSI/Link.

Files:

- `mac/driver/mp_init.c`: the INIT.
- `mac/driver/mp_drv.c`, `mp_drv.h`: the driver (Open/Close/Control/Status, poll, delivery).
- `mac/driver/mp_scsi.c`, `mp_scsi.h`: the core's `mp_bus` on the SCSI Manager.
- `mac/driver/mp_drv_stubs.s`, `mp_drv_ph.s`: entry stubs and the protocol-handler call.
- `mac/driver/moofport.r`: resources.
- `mac/moofstat/`: MoofStat, prints the driver counters to `MoofStat-<run id>.out`.

## How it works

- The INIT does nothing if Shift is held, or if a `.ENET0` is already open (AppleTalk/EtherTalk
  opened DaynaPORT at boot). Otherwise it installs a pointer-based `.ENET0` unit.
- Open finds the DaynaPORT by INQUIRY, enables it, reads the MAC, and starts a Time Manager task
  that polls every 1 tick.
- Each poll runs multi-packet READs through the portable core. Frames are delivered to the protocol
  handlers after each READ, or in the middle of a READ when the 3-slot ring fills (Snow sends up to
  8 packets per READ whatever the allocation says).
- EWrite sends at once, zero copy. When the SCSI Manager is busy (for example a handler sends from
  inside delivery) the frame is copied to the TX ring and sent later.
- Handlers are called with A3/A4/D1 as MacTCP expects (D1 counts the 4 CRC bytes), and
  ReadPacket/ReadRest work.
- ESetGeneral and multicast requests return 0. A private Status code 0x4D50 returns the counters
  for MoofStat.

## Measured (Snow, last three commits on the branch)

| | MoofPort | DaynaPORT |
|---|---|---|
| TcpBench echo | 64/64 | |
| bulk receive | 19,589 B/s (19,681 in the first run) | 18,929 B/s |
| bulk send | 7,205 B/s (7,224) | 6,773 B/s |
| EnetBench ICMP echo | 1.45 ticks | |
| sync EWrite | 0.20 ticks | 0.85 |
| interrupt mask in handler | 1 | 6 |
| resident | 15,294 bytes | 16,554 cap |

Code is 7,226 bytes, position independent (`-mpcrel`, PC-relative calls in the stubs), no Retro68
relocator or multi-segment runtime, `--gc-sections` drops the unused core. These are Snow numbers;
Snow does not model SCSI timing, so speed claims wait for hardware (PLAN 1.3).

### Flynn run (2026-11-07, Snow, fresh disk)

Flynn (Flynn Lite from `a local build of Flynn`) connected to 172.17.0.1:17023 through
MoofPort. The banner "MoofPort test host" and "login:" appeared, typed text came back from the
server's echo (Echo Off in Flynn, so every character on screen is the server's), the session closed
and Flynn quit from the File menu, no crash. Echo felt immediate in Snow (no visible lag after each
key). The echoed text showed doubled letters ("helloo moooofpport"); the host log shows the server
received 20 bytes for 14 typed, so the duplicates were generated on the input side (xdotool key
timing in Snow) and TCP carried them faithfully; not a driver fault. MoofStat afterwards:
`polls 12296 busy 148 bus errors 0, rx frames 25, overflow 0, tx now 43 deferred 0 failed 0`.
Screenshots (scratchpad, not in git): `shots/flynn-banner.png`, `shots/flynn-echo.png`.

## Bugs found on the way

1. A polled SCSIRead with `scNoInc` still advanced the address and wrote a whole frame over the
   driver's state. Discards now go to a sink in 16-byte pieces.
2. The DRVR flag `dStatEnable` is 0x0800 (flags are 0x4C00).

## Not done or not tested

- EtherTalk-at-boot path: untested in Snow (the golden image has no AppleTalk).
- Unit-table growth path: untested in Snow (the table has 64 entries there).
- Blind transfers are off.
- The poll is a fixed 1 tick, no adaptive rate (`mp_poll` is not linked).
- ERead and ERdCancel return controlErr.
- Real hardware has not been run.

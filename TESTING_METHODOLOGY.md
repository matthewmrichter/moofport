# Testing methodology

All speed claims come from a real Mac Classic. The Snow emulator does not model SCSI timing, so it
is used only for functional tests.

**Hardware.** Mac Classic (8 MHz 68000, 4 MB), System 6.0.8, MacTCP 2.1, BlueSCSI v2 Wi-Fi on a
2.4 GHz channel 6 link to the router, then 5 GHz to a Linux PC running `scripts/testnet.py`
(TCP/UDP echo, sink and source).

**Tests.** Run on the Mac by MoofBatch:

| Test    | What it does                             |
| ------- | ---------------------------------------- |
| A       | synchronous 1,400-byte TCP sends, 256 KB |
| C       | 4 sends queued, 256 KB                   |
| echo    | 64 round trips of 1 byte                 |
| receive | 1 MB from the PC                         |

UDP marker datagrams before and after each test let the PC's packet capture be split per test.

**AppleShare test.** AfpCopy copies a file (320 KB data fork + 16 KB resource fork) to a mounted
AppleShare volume over EtherTalk and back, and compares every byte. MoofAgent ran it 8 times per
driver, in blocks alternating DaynaPORT and MoofPort (2026-11-10). The server was netatalk on the
test PC. AppleTalk is not IP, so the timing comes from a separate capture filtered on `atalk or aarp`:
each direction runs from the first to the last AppleTalk frame of 500 bytes or more. Gaps of 0.5 s
or more in that span are AppleTalk (ATP) retries after a lost frame; the README gives the rates
with and without them.

**Timing is on the PC**, from packet captures and testnet's connection times, not from the Mac's
`Ticks`, which undercount under interrupt load. Echo is the exception: it uses the Mac's Time
Manager stopwatch. Figures are medians.

**Loss** is counted from gaps in the Mac's IP identification numbers in the capture, so every
lost frame counts, not only TCP retransmits.

**Interleaving.** MoofAgent on the Mac and `moofctl.py` on the PC run tests and switch drivers
unattended, so DaynaPORT and MoofPort alternate in blocks (MoofPort, DaynaPORT, MoofPort,
DaynaPORT) over about two hours. Both drivers see the same radio conditions, which drift
over a session. In the 1.0 measurement DaynaPORT ran 20 MoofBatch passes and MoofPort 8 with its
default settings (about 16,000 and 6,500 full-size frames). Full-size frames lost, with Poisson 95%
intervals: MoofPort 16.6 per 1,000 (13.6-20.0), DaynaPORT 21.6 (19.4-24.0). The drift between
blocks (14-27 per 1,000, for both drivers alike) is larger than the difference between the drivers.

**Emulator.** Snow is used for functional tests: installs, updates, uninstalls and the tools.

**Known limitation.** The Wi-Fi link loses about 1-2% of full-size frames, varying with radio
conditions. MacTCP resends after about 1 s. DaynaPORT is affected equally on the same hardware.

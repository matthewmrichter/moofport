# Compatibility matrix (MoofPort 1.0b1)

Status per configuration. "Snow" rows are functional checks in the emulator (no SCSI timing);
"HW" rows are the real Mac Classic (System 6.0.8, MacTCP 2.1, BlueSCSI v2 v2026.08.17). Dates
2026-11-07/08. Raw hardware-session logs are kept outside the repository; Snow results are in `docs/dev/phase2-results/`.

| Configuration | Snow | Hardware | Notes |
|---|---|---|---|
| MacTCP only, INIT install (MoofPort in the System Folder) | **pass** (1.0b1: TcpBench 64/64 echoes, 741 frames sent, 591 received, 0 errors; MoofStat shows the 1.0 poll settings) | not run since Phase 1 (hw sessions used the System-file install) | the INIT stays out if `.ENET0` is already open |
| MacTCP only, System-file install (MoofPortInstall) | pass (0.3d1-0.3d3) | **pass** (hw2-hw6) | |
| EtherTalk + MacTCP, System-file install | **pass** (1.0b1: boots with EtherTalk selected, AppleTalk opens MoofPort at INIT time, the 1.0 settings active, 16 frames sent, 0 errors) | **pass** (hw2-hw6) | DaynaPORT 1.2.5 crashes at boot with EtherTalk in Snow (`BOOT-CRASH-ETHERTALK.md`) |
| AppleShare over EtherTalk (mount, copy both ways, byte-exact) | pass (TAP + netatalk, 0.2) | **pass** (hw2-hw6, AfpCopy PASS every run) | |
| AppleShare and MacTCP traffic at the same time | | **pass** (hw3, hw4: AfpCopy PASS while TcpBench ran) | needs the yielding tools (run id `both`) |
| MultiFinder on | pass | **pass** (every hardware session) | |
| MultiFinder off (Finder only) | not run | not run | open: nothing in the driver depends on MultiFinder |
| LocalTalk selected (MoofPort installed, AppleTalk on LocalTalk) | **pass** (HeapInfoOpen opens MoofPort after a LocalTalk boot: 17,304 fixed bytes, 0.3d1) | hw1c used DaynaPORT only | MacTCP still uses `.ENET0` |
| No device (BlueSCSI absent), INIT install | **pass** (1.0b1: boots normally; OpenDriver(".ENET0") = -23 openErr; MacTCP reports "not installed"; no hang) | not run | |
| No device, System-file install with EtherTalk | not run | not run | open: AppleTalk would get openErr at boot and fall back; to check |
| Shift at boot skips the INIT install | **open**: in Snow, holding Shift from power-on did not stop the INIT (MoofStat found MoofPort open at unit 52) | not run | Snow's keyboard may not report a held Shift at INIT time. Not part of 1.0: the release ships only the System-file install, which Shift never skips (README). Check before an INIT version is released |
| Install / Update / Uninstall round trip (System file identical afterwards) | **pass** (real-Classic image and golden copy; uninstall while the driver is open) | **pass** (hw2 install, hw3 and hw4 update) | |
| Blind payload reads | pass | **pass** (hw3: 697 full frames checksum-verified, 0 bad; default since 0.3d2) | |
| Blind frame writes (MoofTune `txblind`) | pass | **pass** (hw5: 0 checksum errors, every byte matched) | off by default: no measurable gain |
| BlueSCSI firmware before v2026.01 (single-packet) | host tests (fake device, old-firmware personality) | not run | the core falls back to single-packet READ and unbatched WRITE |
| Memory: resident at most DaynaPORT + 1,024 (decision 4) | **pass** (17,304 fixed bytes, 0.3d1; 1.0b1 model 17,520 under 17,578) | **pass** (hw7: 17,530 fixed bytes, 48 under the cap) | no room left for growth |

hw7 (2026-11-08): memory, raw rate, TcpBench and AfpCopy done (`BENCH.md` "hw7 findings"); the day
of normal use is still to come.

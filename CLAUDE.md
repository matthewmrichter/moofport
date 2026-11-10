# CLAUDE.md

Guidance for Claude Code (and other coding agents) working in this repository.

MoofPort is an open-source `.ENET0` Ethernet driver for the BlueSCSI v2 Wi-Fi (DaynaPORT emulation) on a Mac Classic
(68000, 4 MB, System 6.0.8, MacTCP 2.1). It replaces DaynaPORT SCSI/Link 1.2.5 (`DRVR 47 ".ENET0"` in the System file)
for speed and latency, stays fully MacTCP compatible, and must not use more resident memory than DaynaPORT. 1.0 installs
into the System file with `MoofPortInstall`. The plan, evidence and decisions are in `docs/dev/PLAN.md`; developer docs are in
`docs/dev/` (`INTERFACE.md` is the MacTCP contract, `INSTALLER.md`, `RELEASE.md`, `HARDWARE.md`, `BENCH.md`, `COMPAT.md`,
`RESEARCH.md`). Raw hardware-session logs and session specs are not in the repository. The sibling project MoofTCP
(`../mooftcp`) shares the toolchain and emulator setup; you do not need it except for the Snow files below.

Device protocol sources: BlueSCSI-v2 `lib/SCSI2SD/src/firmware/network.c` (tested against firmware tag `v2026.08.17`;
"old firmware" = before commit `e7d9629`, 2026-01-12) and Snow `core/src/mac/scsi/ethernet.rs`. Cite them (and anything
else) by path or URL; mark unknowns OPEN.

## Repository map

    core/       portable C99 core (prefix mp_): ring, DaynaPORT CDB builders, RX parser, TX, demux, poll, ipck, budget
    mac/driver/ the .ENET0 driver: mp_drv.c (driver, Time Manager poll), mp_scsi.c (core mp_bus on the SCSI Manager),
                mp_init.c (INIT), asm stubs, moofport.r; -DMP_DIAG builds a timing-diagnostic driver (never shipped)
    mac/installer/  MoofPortInstall (System-file DRVR 47 install/update/uninstall)    mac/moofstat/  MoofStat
    mac/mooftune/   MoofTune (poll timing settings, verify mode)    mac/afpcopy/ AfpCopy    mac/common/ runid, shared glue
    mac/*       other test tools: TcpBench, TcpSend, EnetBench, ScsiProbe, CpuMeter, HeapInfo*, SpyENET
    tests/      host tests (ctest) and a fake DaynaPORT device    tools/  pinned tool versions, tools/sit (StuffIt writer, MIT)
    scripts/    build, Snow, release and test-host tooling       docs/  README-level docs, docs/dev, docs/release/ReadMe.txt

## Commands

Host (portable core and tests: gcc + CMake, ASan/UBSan on by default):

    cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure   # re-run cmake for new files
    ./build/test_e2e batch             # one test exe; optional arg = test-name substring
    cmake -S . -B build -DMP_MEM_CAP=12000 -DMP_RX_SLOTS=3 -DMP_TX_SLOTS=1   # budget check for other settings

Mac (Retro68 in Docker) and Snow:

    scripts/r68 <cmd>...               # run a command in the pinned Retro68 container (repo at /src)
    scripts/build-mac.sh <Target>      # -> build-mac/<dir>/<Target>.bin (MacBinary); reconfigures every call
                                       # Targets: MoofPortInstall MoofStat MoofTune AfpCopy TcpBench MoofPort (INIT, dev only) ...
    scripts/fresh-disk.sh              # $MT_HOME/disks/golden.hda -> $MP_HOME/disks/work.hda (hash-checked)
    scripts/deploy.sh <x.bin>... --to ":Folder:"   # copy onto work.hda; refuses while Snow runs
    scripts/snow.sh start|stop|status|shot <png>   # Snow on a private Xvfb display
    scripts/phase0-run.sh [--no-spy] [--no-testnet] [--keep-disk]   # test kit on a fresh disk + Snow
    scripts/testnet.py                 # host TCP/UDP echo, discard, banner and bulk services (17007...17031)
    scripts/release.sh [--force] [--no-build] <ver>   # dist/MoofPort-<ver>.{dsk,sit,sit.hqx,sha256}; docs/dev/RELEASE.md
    scripts/release-verify.sh [--self-test] <ver>     # reads every format back with independent tools
    scripts/stage-hw.sh [--spec s.md] <session> <x.bin>...   # stage a hardware-test kit in dist/hw/ (docs/dev/HARDWARE.md)
    scripts/icons.py rez|preview out.png   # mac/icons/moofport.icn.txt -> icons.r

The guest reaches the host as `172.17.0.1` in Snow (NAT link; `MP_ETH_LINK=Down` for a dead link). Each script's header
explains its options.

## Toolchain and Snow

Host tests need only gcc and CMake. Mac builds need Docker: Retro68 runs in a container pinned by the digest in
`tools/retro68.digest` (`scripts/r68`). Anything needing `sudo` or Docker group membership is set up by whoever sets up the machine.
The Snow emulator tests need your own ROM and Apple software (never in git):

- `MT_HOME` (default `~/.local/share/mooftcp`): holds `snow/snowemu` (pinned Snow), `rom/MacClassic.rom` and the golden
  image `disks/golden.hda` (System 6.0.8 + DaynaPORT SCSI/Link 1.2.5 + MacTCP 2.1, 10.0.0.2/8), used **read-only**. The
  layout matches MoofTCP's `MT_HOME`; build the tree yourself if you do not use MoofTCP.
- `MP_HOME` (default `~/.local/share/moofport`): MoofPort's own state: `disks/work.hda`, the Snow workspace, PID files, logs.
- Snow runs on a private Xvfb display in `:140`-`:149` (default `:143`; set `MP_DISPLAY`), never on your desktop. Use
  `scripts/snow.sh`; it stops only the Xvfb it started. Other knobs: `MP_ETH_LINK`, `MP_DISK`, `MP_PRAM`, `MP_NO_ETH=1`.
- Never edit a disk image while Snow runs. Stop Snow with `snow.sh stop` (recorded PID), never `pkill -f snowemu`. Never
  mount or write `golden.hda`; only `work.hda`, with Snow stopped.

## Rules and gotchas

- Nothing Apple-copyrighted or large in git: ROMs, System files, MacTCP, DaynaPORT, disk images, built binaries (`.bin`,
  `.hqx`, `.dsk`, `.img`, `.hda`). `.gitignore` covers the usual suspects.
- Speed claims come only from hardware measurements; Snow does not model SCSI timing (PLAN 1.3).
- MacTCP compatibility is at the `.ENET0` API boundary (csCodes, PB layout, error codes, the protocol handler register
  contract incl. D1 counting the 4 CRC bytes). Internals are our own design; where DaynaPORT's behaviour is a bug (sync
  EWrite hangs, interrupt mask 6) do the sane thing as long as MacTCP cannot tell.
- The driver is position-independent and not relocated: no libgcc helper calls (`__divsi3`, `__mulsi3`, ...; they link at an
  absolute address and crashed it once). `release.sh` checks with `nm`; avoid 32-bit division and multiplication by variables.
- Mac Ticks are unreliable for timing under load. Time on the PC (host capture, `testnet.py` logs), not with Ticks.
- Resident memory: at most DaynaPORT's 16,554 bytes + 1,024 = **17,578** (PLAN 2.4, decision 4). `test_budget` enforces it for
  the default settings; release builds must fit. Trim only where performance does not suffer; never fewer ring slots to save memory.
- Test tools ask for a run id (`mac/common/runid.c`) and write `<Tool>-<id>.out` beside themselves. The id is kept in the text
  file `MoofPort Run`; a first line starting with `!` is used without asking, which keeps Snow runs unattended.
- The INIT build (`MoofPort`, `mp_init.c`) is a developer option only. Never run it beside a MacTCP/AppleTalk-opened
  DaynaPORT `.ENET0` (the INIT stays out if one is already open). Users get the System-file install.

## Portable core (`core/`)

Global constraints (prefix `mp_`):

- C99, no GNU extensions (the one exception, `MP_BARRIER`, is `__GNUC__`-guarded). Must build with Retro68's
  `m68k-apple-macos-gcc -m68000`: no 64-bit math, no division or modulo on hot paths, 2-byte alignment safe (never overlay
  structs on packet bytes).
- No `malloc`, no mutable globals (`static const` tables are fine); all state lives in caller-provided structs.
- All multi-byte packet access goes through `mp_rd16/mp_rd32/mp_wr16/mp_wr32` (`mp_util.h`); widen before shifting.
- The uint32 ms clock is compared only with `mp_time_before` / `mp_time_since` (wrap-safe).
- No blocking or sleeping; every call finishes in bounded time.
- Flags `-std=c99 -Wall -Wextra -Werror -g`; tests run with ASan+UBSan (`MP_SANITIZE`, default ON), any report is a failure.
  Portability smoke check: `m68k-linux-gnu-gcc -m68000 -Os -std=c99 -Wall -Wextra -Werror -pedantic -c core/*.c` builds clean
  with no libgcc helper calls (only `memcpy`/`memset` undefined).

Module map (umbrella header `core/mp.h`):

- `mp_util.h`: byte access, wrap-safe time, `MP_BARRIER`, result codes `MP_OK`/`MP_E_*`.
- `mp_ring.[ch]`: fixed slot ring (slot 1524 = 2-byte length + 2 reserved + 1520 data). Count set at init (1..32). Single
  producer / single consumer, safe for interrupt vs task. Used for the RX slots and the TX deferral queue.
- `mp_dev.[ch]`: DaynaPORT CDB builders (READ 0x08, WRITE 0x0A, 0x09 MAC+stats, 0x0D multicast, 0x0E enable, 0x1C Wi-Fi),
  INQUIRY match, READ allocation policy, Wi-Fi codecs. `mp_dev_rx.c`: `mp_rx`, the incremental READ-response parser
  (`mp_rx_want` / `mp_rx_got` / `mp_rx_next` / `mp_rx_end`; `mp_rx_poll` runs a whole READ over an `mp_bus`).
  `mp_dev_tx.c`: `mp_tx` (`mp_tx_send` zero-copy single-frame WRITE; `mp_tx_queue` + `mp_tx_flush` batched).
- `mp_demux.[ch]`: EtherType -> protocol handler table, EAttachPH/EDetachPH semantics (-94 `LAPProtErr`; type 0 = every
  802.3 frame), counters. `mp_poll.[ch]`: adaptive poll interval, all values in ms and tunable.
- `mp_ipck.[ch]`: IPv4/TCP/UDP checksum check of a received frame (verify mode, the blind-read gate).
- `mp_budget.[ch]`: resident bytes from the slot counts (code 9400 + slots x 1524 + state 500; 3 RX + 2 TX = 17,520).

`mp_bus` is the glue contract: `begin` (SCSIGet + SCSISelect + SCSICmd; `MP_E_BUSY` when busy, nothing done), `xin`/`xout`
with a hint (`MP_XF_POLLED`, `MP_XF_BLIND`, `MP_XF_NOINC`), `in_data`, `end` (SCSIComplete -> status byte). Every transaction
calls `end` exactly once after a successful `begin`.

Device facts the core relies on (all pinned by tests):

- RX len counts 4 CRC bytes; new fw pads frames to 60 before the CRC, old fw reports len < 64 as 64, Snow pads to 64.
  Flags 0x10 = more queued, but v2026.08.17 and Snow clear it on the last packet of every READ; only old fw leaves it set.
- v2026.08.17 multi-packet READ stops when `total + 1530 > alloc` or `total >= 3060`; Snow sends up to 8 whatever alloc says.
  `mp_dev_read_alloc(k, 1)` (= 70(k-1) + 1530) never yields more than k packets from the firmware.
- The firmware's 20-slot RX queue reads as EMPTY when the 20th unread frame arrives: it holds at most 19, so poll before that.
- Old fw WRITE with CDB[5] = 0x80 reads exactly CDB len + 8 bytes.

Tests (`tests/`, harness `tests/t.h`, `T_MAIN` takes at most 32 tests per file): one `test_*` per core module plus `test_e2e`
(codec + ring + demux + TX against `tests/fakedev.[ch]` in three personalities: `FD_FW_OLD`, `FD_FW_2608`, `FD_SNOW`).

Review focus (every core change):

- Hot paths (per-frame RX parse, ring ops, demux lookup, TX) get no new loops beyond the per-segment copy and no logging.
- Every counter in a stats struct is bumped exactly once per event it names.
- Ring indices: slot bytes first, `MP_BARRIER`, then the index store.
- Multi-byte packet access only via `mp_rd*/mp_wr*`; clock values only via the wrap-safe helpers.
- Memory is budgeted: keep `test_budget` green for the default settings.

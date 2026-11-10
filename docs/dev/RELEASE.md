# Release build and verification

Adapted from MoofTCP's `docs/dev/RELEASE.md` (same tools, same verification method). A MoofPort
release is three files plus a checksum file, all holding the same four items:

| File | What | Made by |
|---|---|---|
| `dist/MoofPort-<v>.dsk` | 800K HFS image, volume `MoofPort`: `MoofPortInstall`, `MoofStat`, `MoofTune`, `Read Me` | hfsutils (`hformat`, `hcopy -m`) |
| `dist/MoofPort-<v>.sit` | StuffIt 1.5.1-compatible archive of the same four files, both forks, type/creator, Finder flags; flat (no folder entry) | `tools/sit/mksit` |
| `dist/MoofPort-<v>.sit.hqx` | BinHex 4.0 of that `.sit` (one file `MoofPort-<v>.sit`, type/creator `SIT!`/`SIT!`, data fork only) | macutils `binhex` |
| `dist/MoofPort-<v>.sha256` | `sha256sum` lines of the three files above (`sha256sum -c MoofPort-<v>.sha256`) | `sha256sum` |

| Item | Type/creator | Finder flags | From |
|---|---|---|---|
| `MoofPortInstall` | APPL/MPIN | 0x2000 (hasBundle) | `build-mac/MoofPortInstall.bin` |
| `MoofStat` | APPL/MPST | 0x2000 | `build-mac/MoofStat.bin` |
| `MoofTune` | APPL/MPTU | 0x2000 | `build-mac/MoofTune.bin` |
| `Read Me` | ttro/ttxt | 0 | `docs/release/ReadMe.txt`, CR line endings, no resource fork |

`dist/` is gitignored; the payload and the files are generated, never committed. Nothing Apple or
DaynaPORT is in them: the installer carries MoofPort's own driver (`'MPDV'` 128) and keeps whatever
DaynaPORT driver is already on the Mac.

## Differences from MoofTCP

- **Payload.** MoofTCP ships one installer plus a Read Me. MoofPort ships three applications plus a
  Read Me: MoofStat and MoofTune are part of the user documentation (troubleshooting asks for a
  MoofStat file; MoofTune is in the README's Tools section), so every format carries all four.
- **The BinHex file is `MoofPort-<v>.sit.hqx`, BinHex of the `.sit`, not of the installer alone.**
  MoofTCP's `.hqx` is BinHex of its single installer and leaves the Read Me out. For MoofPort that
  would drop MoofStat and MoofTune from the format the README recommends by default, and BinHex
  holds only one file. BinHex of the archive is the classic `.sit.hqx` download: text-safe (any
  transfer, mail or web form keeps it intact), it carries the `.sit`'s type and creator, and StuffIt
  Expander and StuffIt Lite decode and expand it in one step. The hardware test kits ship this
  way (`scripts/hw-bundle.py`), expanded with StuffIt Lite 3.6 on the Classic. The
  name ends in `.sit.hqx` so it says what is inside; README's Download table matches.
- **Checksum file.** MoofTCP's `release.sh` writes none; its GitHub release has a hand-made
  `SHA256SUMS`. MoofPort's `release.sh` writes `MoofPort-<v>.sha256` (same `sha256sum` format) and
  `release-verify.sh` checks it, so the checksums that get published are the verified ones. Upload
  it with the release (renaming it `SHA256SUMS` is fine; the lines name the files, not the list).
- **No variants, no release switches.** MoofTCP's Lite variant, its `release.conf` (`DIAGLOG_TEXT`)
  and the embedded-build checks have no MoofPort counterpart. The MoofPort counterpart of "not a debug
  build" is "not the diagnostic timing build" (`MP_DIAG`, below).
- **No folders in the `.sit`.** MoofPort's `mksit` can write folders, but an archive with folder
  entries hung StuffIt Lite 3.6 on a real Classic (2026-11-06; `scripts/hw-bundle.py`), so the
  release stays flat like MoofTCP's. The README tells users to make a "MoofPort" folder first.

## Tools needed (host)

`docker` (the pinned Retro68 image, `scripts/r68`), hfsutils (`hformat hmount hcopy hls hattrib
humount`), macutils (`binhex hexbin`), `unar`/`lsar` (The Unarchiver), `gcc`, `python3`,
`sha256sum`. Debian: `sudo apt install hfsutils macutils unar` (a step for whoever sets up the machine). Both scripts check with
`command -v` and stop if one is missing.

## Build

    scripts/release.sh 1.0               # builds, makes the files, runs release-verify.sh
    scripts/release.sh --force 1.0       # replace existing dist/MoofPort-1.0.*
    scripts/release.sh --no-build 1.0    # reuse build-mac/ (the release-build checks still run)

Steps:

1. `scripts/build-mac.sh MoofPortInstall`, `MoofStat`, `MoofTune` (`build-mac.sh` configures with
   `-m68000` only; the diagnostic targets `MoofPortDiag`/`MoofPortDiagDrv` are separate and never
   used here).
2. Release-build checks:
   - `CMakeCache.txt` `CMAKE_C_FLAGS` has no `MP_DIAG`;
   - `nm build-mac/MoofPortDrv.flt.gdb` shows no libgcc helper (` T __*si3`/`__*di3`: the driver
     is position-independent and not relocated, so a helper at an absolute address crashes it; a
     helper crashed an early diagnostic build) and no `mp_diag` symbol;
   - `scripts/relvers.py`: the installer's `vers` 1 is `<v>` / "MoofPort `<v>` installer", its code
     has the log line "MoofPortInstall `<v>`" + CR exactly once (`MP_INST_VER`, first line of
     `Installer-<name>.out`), the word "diagnostic" is nowhere in it (the `MP_DIAG` build logs
     "MoofPortInstall `<v>` diagnostic"), and `'MPDV'` 128 is the 50-byte driver header plus
     `build-mac/MoofPortDrv.flt` byte for byte;
   - each app's MacBinary name, type APPL, creator and hasBundle bit.
3. `Read Me` MacBinary made from `docs/release/ReadMe.txt` (must be plain ASCII, at most 64
   columns; LF -> CR; ttro/ttxt).
4. `.dsk`: `dd` 800 KiB, `hformat -l MoofPort`, `hcopy -m` the four files.
5. `.sit`: `tools/sit/mksit` built with gcc (`build/sit/mksit`), the four files, flat.
6. `.sit.hqx`: the `.sit` wrapped as MacBinary `MoofPort-<v>.sit` (SIT!/SIT!), `binhex`, then
   macutils' own first comment line is replaced by the standard `(This file must be converted with
   BinHex 4.0)` that `unar` needs.
7. `MoofPort-<v>.sha256`; the files are moved into `dist/` only after all were made, then
   `release-verify.sh` runs. Existing outputs are refused without `--force`.

hfsutils keeps one global "current volume" in `$HOME/.hcwd`. `release.sh` runs it with `HOME` set
to its staging directory and `relverify.py` with a private temporary `HOME`, so neither clashes with
`deploy.sh` (which uses `$MP_HOME`) or a MoofTCP job.

`scripts/hw-bundle.py` (hardware kits, `stage-hw.sh --bundle`) is unchanged and separate. The
previous `release.sh` (one `dist/release/MoofPort-<v>.sit.hqx` through `stage-hw.sh` and
`hw-bundle.py`) had no other caller.

## The .sit writer

`tools/sit/` is MoofTCP's copy of SitStill's MIT-licensed Classic SIT writer. `classic_sit_writer.c`,
`stream.c`, `sit_crc.c`, `classic_sit_internal.h` and `sitstill/*.h` are identical to MoofTCP's;
`mksit.c` is MoofTCP's driver extended with folder records (`+Name` ... `-`), unused by the release
(`tools/sit/NOTICE`).

## Verify

    scripts/release-verify.sh 1.0               # every format with independent readers
    scripts/release-verify.sh --self-test 1.0   # the artifacts pass AND broken copies fail

Readers (`scripts/relverify.py`):

- `.sit.hqx` with macutils `hexbin`: exactly one file, `MoofPort-<v>.sit`, SIT!/SIT!, no resource
  fork, data fork byte for byte the `.sit` in `dist/`; that decoded `.sit` is then read with `unar`;
- `.sit.hqx` with `unar`/`lsar` directly (they look through the BinHex into the archive);
- `.sit` with `unar`/`lsar`;
- `.dsk` with `hmount`/`hls -a`/`hcopy -m`: exactly 819200 bytes, volume `MoofPort`, exactly the
  four files at the root (invisible files count);
- `.sha256` recomputed: one line per artifact, all matching.

Each file found is compared with `build-mac/<app>.bin` (and the Read Me derived from
`docs/release/ReadMe.txt`, not copied from `release.sh`): name, type, creator, Finder flags (all bits
but 0x0100 kHasBeenInited), both fork lengths, sha256 of both forks. Dates are not compared. The
`relvers.py` checks run again on `build-mac/MoofPortInstall.bin`.

The self-test first requires the real artifacts to pass, then requires each of these to fail: one
flipped driver byte in the `.sit` and in the `.dsk`, the Read Me type code changed in the `.dsk`
(`hattrib`), one changed BinHex character (hexbin and unar), CRC-valid wrong archives (MoofStat with
creator XXXX, MoofTune without the bundle bit, no Read Me, a Read Me with one word changed, a
`.sit.hqx` of an archive without MoofTune, a `.sit.hqx` whose inner name is another version's, a
`.sit.hqx` that is not the `.sit` beside it), a `.dsk` one byte short, a `.dsk` with an extra
invisible file, a checksum file with one wrong digit or a missing line, and installers that
`relvers.py` must reject (wrong version, one driver byte changed, log line missing, the word
"diagnostic").

## Snow check

Each release file is unpacked in Snow the way a user gets it, on its own fresh copy of the golden
image (System 6.0.8, DaynaPORT SCSI/Link 1.2.5, MacTCP 2.1), then installed, restarted and checked
with MoofStat. Use a private disk under `$MP_HOME` (`MP_DISK=...`) so `work.hda` is not touched.

- `.dsk`: `scripts/snow.sh start --floppy <copy.dsk>` (Snow asks "Save MOOF copy?" for a raw
  image: click Load original, so the copy stays raw, as in MoofTCP's RELEASE.md). The floppy
  `MoofPort` appears under the hard disk; make a "MoofPort" folder on the hard disk, drag the four
  files into it, eject, run MoofPortInstall.
- `.sit` / `.sit.hqx`: no StuffIt Expander or StuffIt Lite for System 6 is kept with the project.
  StuffIt 1.5.1 (System 6) decodes BinHex (Other > Decode BinHex File...) and expands a `.sit`; a
  copy is in a local SitStill corpus (`a local SitStill corpus, macintoshrepository/
  stuffit-151.app.macbin`). Deploy it, and the release file wrapped as MacBinary (`.sit.hqx` as
  TEXT/ttxt, `.sit` as SIT!/SIT!) into `:MoofPort:`, with `deploy.sh` while Snow is stopped.

Snow input: Snow's guest cursor trails the host pointer, so menu items are reached by dragging
straight down from the title in 20 px steps and holding about 2 s before the release; hold the
button about 0.1 s for clicks. Command keys are `xdotool key super+<k>` (then `keyup super`).

1.0 (2026-11-10, Snow on `:144`, disks `rel10-{hqx,sit,dsk}.hda`): all three release files expanded
in the guest (StuffIt 1.5.1: Decode BinHex gave `MoofPort-1.0.sit`, SIT!/SIT!, 214416 bytes; Open +
Select All + Extract + Save All gave the four files; the `.dsk` mounted as floppy "MoofPort" and
the Finder copied the four files). On each: MoofPortInstall, run name, Install, "MoofPort is
installed", Special > Restart, MoofStat. Results on every disk: `Installer-<run>.out` shows DRVR 47
replaced by MoofPort's 9324-byte driver and DaynaPORT's 12,512-byte driver kept as DRVx 47; the
System file's DRVR 47 is the 50-byte header + `MoofPortDrv.flt`; MoofStat after the restart: "MoofPort
refnum -48, SCSI ID 3, open 1". The files copied back off the disks have the build's resources and
data byte for byte (only Finder-written resource-header bytes and kHasBeenInited differ).

## Publishing (not scripted)

Pushing and the GitHub release are manual steps. Upload `MoofPort-<v>.dsk`, `.sit`, `.sit.hqx` and
`.sha256` as release assets, with notes in the style of MoofTCP's `dist/release-1.0/RELEASE_NOTES.md`
(Download table, highlights, tested, known limitations). CI (if added, like MoofTCP's host-test
workflow) runs on push and is not part of the release build.

## Notes and limits

- The `.sit.hqx` is LF-terminated: valid BinHex and safe for text-mode transfers.
- The `.dsk` is a raw 800K HFS image (no Disk Copy header). Floppy emulators and Mac emulators take
  it as is; Disk Copy 4.2 needs a converted image. BlueSCSI hard-disk images do not use it.
- Release files are not reproducible byte for byte: the `Read Me`, the `.sit.hqx` wrapper and the
  HFS volume carry the build date.
- `hfsck` is not installed on this host; the `.dsk` is checked by `hmount` (MDB) plus `hcopy -m` of
  every file listed with `hls -a`.

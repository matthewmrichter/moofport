# AfpCopy: the AppleShare round trip (Phase 2)

AfpCopy (`mac/afpcopy/afpcopy.c`) is the AppleShare test for PLAN Phase 2. AppleShare runs
AFP over ASP/ATP/DDP, and with EtherTalk selected all of that goes through `.ENET0`. AfpCopy
copies a file of a few hundred KB to a mounted AppleShare volume and back with the File Manager,
compares every byte, and logs MoofPort's counters (private Status 0x4D50) around each copy. A
NBP lookup or a Chooser listing only moves a few small frames; this test moves enough data to
exercise the receive and transmit paths.

    scripts/build-mac.sh AfpCopy        # build-mac/AfpCopy.bin

## What it does

1. Finds the server volume: the one named `volume=` in `AfpCopy.cfg`, else the first mounted
   volume with a non-zero file-system id (HFS is 0, AppleShare is not). It waits up to `wait`
   seconds for the volume to appear.
2. Writes `AfpCopy src` next to itself: `kb` KB of data fork and `rsrc` KB of resource fork, both
   a reproducible xorshift32 pattern.
3. Copies it to `AfpCopy test` in the volume's root (both forks), timed, then back to
   `AfpCopy back`, timed.
4. Compares `AfpCopy back` with the pattern. For the resource fork, bytes 16-127 are skipped:
   that header area is reserved for the system, and System 6 writes the file's name there
   (seen in Snow at offset 47).
5. Writes `AfpCopy-<run id>.out` (runid.c; `!id` in `MoofPort Run` skips the dialog) and shows
   PASS or FAIL. The last line of the file is `RESULT PASS` or `RESULT FAIL <why>`.

The server copy is kept. Check it from the host (netatalk stores the data fork as the plain
file):

    scripts/afp-verify.py /path/to/share/"AfpCopy test" [kb]

`AfpCopy.cfg` (optional, next to the app, one setting per line):

    volume=MoofShare
    kb=320
    rsrc=16
    wait=60
    keep            # keep AfpCopy src / AfpCopy back too

## Mounting the volume without the Chooser each time

System 6 has no call to mount a server volume (`PBVolumeMount` came with System 7), so AfpCopy
cannot log in by itself. Instead, mount the volume once in the Chooser and tick its box in the
volume list ("Checked volumes will be opened at system startup"). AppleShare then offers to save
the name only, or the name and password. Save both, and the volume mounts at every boot before
the Finder starts. AfpCopy's `wait` covers a volume that is still mounting.

- The password is at most 8 characters (the AppleShare 2.0.2 login dialog refuses more).
- In Snow, use a test account on a test server only, never a real password.

## Results so far

- Snow, against the local disk (`volume=Mac Classic`, 2026-11-07): PASS, 320 KB data plus 16 KB
  resource fork, the server-side CRC matched `afp-verify.py`. This checks AfpCopy itself; no
  network traffic is involved.
- Snow, AppleShare over EtherTalk through MoofPort (2026-11-07): **PASS**. Real-Classic System 6.0.8
  image with EtherTalk selected, MoofPort as DRVR 47 in the System file (`scripts/sysdrvr.py`),
  Snow bridged to the TAP device `tapmp`, netatalk 4 (`moofport-afp` test container, atalkd on
  `tapmp`). Logged in from the Chooser, mounted MoofShare, ran AfpCopy with defaults.
  `phase2-results/AfpCopy-snow-2026-11-07.out`:

  | direction   | bytes   | time   | rate         | frames rx / tx | errors |
  |-------------|---------|--------|--------------|----------------|--------|
  | to server   | 344,064 | 3.10 s | 110,988 B/s  | 262 / 797      | none   |
  | from server | 344,064 | 2.43 s | 141,396 B/s  | 613 / 176      | none   |

  Both forks came back identical; `afp-verify.py` on the server's file: identical, CRC-32
  10baea0c. No overflow, aborted READs, bad frames, failed or deferred EWrites, or bus errors.
  147 frames were delivered mid-READ (`drained`, the Snow 8-packet case). Snow does not model
  SCSI timing, so the rates say nothing about the hardware (PLAN 1.3).
- Earlier the same day, before AfpCopy existed: NBP, the Chooser listing, login, mounting, the
  volume listing and a Finder Duplicate on the server worked.
- Still open: the same run on the real Classic against a real AppleShare server (hw2), and with
  MacTCP traffic at the same time (PLAN Phase 2 AppleShare row; MacTCP was loaded but idle here).

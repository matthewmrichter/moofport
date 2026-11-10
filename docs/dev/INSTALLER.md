# MoofPortInstall: MoofPort in the System file (Phase 2)

`mac/installer/installer.c` is the Mac-side counterpart of `scripts/sysdrvr.py`. With EtherTalk
selected, AppleTalk opens `.ENET0` from the System file before any extension INIT runs
(`BOOT-CRASH-ETHERTALK.md`), so MoofPort has to be DRVR 47 in the System file to serve
AppleTalk and MacTCP.

    scripts/build-mac.sh MoofPortInstall    # build-mac/MoofPortInstall.bin

- **Install:** DaynaPORT's DRVR 47 ".ENET0" is kept unchanged as type `DRVx` 47 (same id, name,
  attributes and bytes). MoofPort's driver, carried in the installer as `MPDV` 128 (built from
  the same `mac/driver/mp_drvr.r` as the INIT version's DRVR 128), is added as DRVR 47
  ".ENET0", sysheap + locked. INIT-version MoofPort files (type INIT, creator MPRT) in the
  System Folder get file type `INIx`, so the two never both run.
- **Fresh install:** a System file with no DRVR 47 and no `DRVx` 47 (DaynaPORT never installed)
  gets MoofPort's driver as DRVR 47 ".ENET0" the same way, with nothing kept. EtherTalk then
  needs only Apple's AppleTalk Phase 2 (Network Software Installer 1.4.5, or the DaynaPORT
  disk's "AppleTalk v58.0.0 for System 6"); neither needs DaynaPORT's driver (Snow, 2026-11-08).
- **Uninstall:** our DRVR 47 is removed and `DRVx` 47 becomes DRVR 47 again. After a fresh
  install there is no `DRVx` 47: our DRVR 47 is removed and the System file has no ".ENET0".
- **Update (0.3d1 and later):** when MoofPort is installed the alert offers Update, Uninstall or Cancel
  (Cancel is the default). Update replaces our DRVR 47 with the installer's driver and leaves
  `DRVx` 47 alone. Snow: Update on the real-Classic image left all 426 resources identical (same
  version) and logged the replacement.
- It acts on the running System file, only after a click in a confirm alert (Cancel is the
  default). It refuses when the System file is not in a state it knows (a DRVR 47 that is not
  ".ENET0", a `DRVx` 47 beside something that is not MoofPort, a `DRVx` 47 with no DRVR 47) or the
  startup disk has less than the new resources plus 32 KB free. New resources are allocated in
  the system heap, because the System file's resource map outlives the application. A driver
  that is open now (AppleTalk, MacTCP) is taken out of the map only, not out of memory, so it
  keeps running until the restart. MoofPort is recognised by its first 50 bytes (header, name,
  stubs): a running driver's block differs from the installer's copy past that.
- Log: `Installer-<run id>.out`.

## Tests (Snow, 2026-11-07)

| Image | Steps | Result |
|---|---|---|
| Real-Classic System 6.0.8 copy (`et.hda`, EtherTalk) | install (LocalTalk boot); restart with EtherTalk, MoofStat: MoofPort open; uninstall while AppleTalk has it open | install changed only DRVR 47 and added DRVx 47; after uninstall all 425 resources and the data fork identical to before |
| Golden copy (`work.hda`, MoofPort INIT in the System Folder) | install; restart; TcpBench through MacTCP; uninstall while MacTCP has it open | INIT retyped INIx; TcpBench echo 64/64, rx 19,016 B/s, tx 7,152 B/s (same as Phase 1); after uninstall all 357 resources and the data fork identical |
| Clean System 6.0.8 + Apple Network Software Installer 1.4.5 (AppleTalk 58.1.4, EtherTalk 2.5.6), no DaynaPORT, EtherTalk selected (2026-11-08) | fresh install; restart; MoofStat, HeapInfoOpen, TcpBench; Cancel and Update at the installed alert; Uninstall; restart; Cancel at the install alert | before install and after uninstall AppleTalk falls back to LocalTalk with an alert and the Finder starts; after install no alert, AppleTalk opens MoofPort at boot (16 frames sent), TcpBench echo 64/64; Update kept DRVR 47 9388 bytes; Uninstall left no DRVR 47 (MoofStat: OpenDriver -43, MacTCP lists no connection); both Cancels changed nothing |
| Golden copy (DaynaPORT, regression, 2026-11-08) | install; restart; TcpBench; uninstall | DRVx 47 kept (12,512 bytes), TcpBench echo 64/64; after uninstall all 357 resources and the data fork identical to before |

"Identical" is per resource (type, id, name, attributes, sha1 of the bytes) and the data fork:
the Resource Manager rewrites the map, so the file's bytes are not compared. The Finder kept
running after each uninstall of a driver in use.

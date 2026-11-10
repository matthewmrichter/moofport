# SpyENET

A pass-through `.ENET0` shim for Phase 0. It sits between MacTCP 2.1 and the real DaynaPORT
SCSI/Link driver, logs every Device Manager call MacTCP makes and every inbound frame the real
driver delivers, and forwards everything so the network keeps working. Results are in
`docs/dev/INTERFACE.md`.

## Pieces

| File | What |
|---|---|
| `spyenet_init.c` | the INIT: installs the spy as a pointer-based `.ENET0` unit |
| `spy_drv.c` | the spy driver: logging and forwarding; owns the log ring (resident globals) |
| `spy_drv_stubs.s` | DRVR entry dispatch, the async-forward completion, the wrapped protocol handler |
| `spyenet.r` | packages INIT 128 and DRVR 128 (`.ENET0`) into the extension file |
| `spydump.c`, `spydump.r` | SpyDump: reads the ring and appends it to `SpyENET.log` |

## How it works

1. At boot the INIT loads our `DRVR 128 ".ENET0"`, detaches and locks it, and calls its boot
   stub once to relocate the flat Retro68 block. Then it installs it by hand: a pointer DCE
   built in the system heap, put in the first free unit-table entry from unit 52 up
   (refnum = ~unit). This is not `DrvrInstall`. Shift held skips all of this.
2. The Device Manager searches the unit table by name before the `DRVR` resources, so
   MacTCP's `OpenDriver(".ENET0")` reaches the spy.
3. The INIT does NOT install the real driver: DaynaPORT cannot be opened at INIT time (its poll
   reads through CurStackBase, which is -1 until the first application runs). Instead, at the
   spy's first open, `spy_real_open` hides the spy's own name, lets the normal resource path load
   and open `DRVR 47` (unit 47, refnum -48, as without the spy), then renames the loaded copy's
   header name to `.DNET0` and restores the spy's name.
4. Every spy call is logged into a ring of 1024 slots and forwarded to the real driver:
   - EWrite (async, must stay async: a sync EWrite from a handler or masked context hangs) is
     forwarded on a copied PB with `PBControlAsync`. Our completion copies the result back and
     `jIODone`s the original. The spy's Control returns "complete later".
   - EAttachPH is wrapped: MacTCP's handler is saved and `spy_ph` is attached in its place.
     On each delivery `spy_ph` logs EtherType, SR and D1, then chains to MacTCP's handler with
     every register intact. A depth counter marks EWrites made from inside a handler.
   - all other calls are forwarded synchronously.
   - Close does not close the real driver.
5. **SpyDump** opens `.ENET0`, makes a private Status call (csCode 0x1F00) that returns the ring
   pointer, appends one line per entry to `SpyENET.log` next to the app, and drains the ring so
   the next run continues. If the magic is wrong it reports that SpyENET is not installed.

When the ring is full new entries are dropped and counted; the `count:` lines cover every event.
Serialisation caveat: the spy queues EWrites on its own unit, so logged depth and SR describe when
the spy ran the call (see INTERFACE.md section 6).

## Verified in Snow (2026-10-31)

- The INIT loads, the spy is installed, and the Mac boots (Mac Classic 4 MB, System 6.0.8).
- MacTCP 2.1 opens the spy by name, the real DaynaPORT driver is opened behind it and renamed.
- TcpBench (echo, bulk receive, bulk send) runs through the spy with no hang or crash.
- Async EWrite forwarding, handler chaining and SpyDump all worked (742 EWrites, 511 logged
  handler deliveries). Spy overhead is negligible (INTERFACE.md section 8).

## Not verified on real hardware

- Everything above on a real Mac Classic with a BlueSCSI. Snow is not the real SCSI timing and
  does not model bus errors.
- Behaviour at MacTCP shutdown or control-panel changes (not exercised in Snow either).
- Memory: the spy adds a resident block of about 44 KB (33 KB of it the ring) and its DCE to the
  system heap, and `sysz` reserves 64 KB for that. It is a diagnostic and is never shipped.

Be ready to boot with Shift held (extensions off) and remove the file if a Mac hangs.

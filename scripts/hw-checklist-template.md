# MoofPort hardware session: {{SESSION}}

Date staged: {{DATE}}

Read this on the host. Do each step in order. If something goes wrong, take a photo and go on.

## Files

{{FILES}}

Copy one format of each file to the BlueSCSI shared folder. Try `.bin` first; if the Mac shows
a plain document with no icon, use the `.hqx` instead.

## Install

{{INSTALL}}

## Try

{{TRY}}

## Undo

{{UNDO}}

## Report back

Fill the blanks (write n/a if a step was skipped, and why). Attach photos where asked.

**Setup**

- Date and time of session: ________
- BlueSCSI firmware version (from log.txt): ________
- log.txt copied from the card after the session (yes/no): ____
- DaynaPORT SCSI/Link version: ________   MacTCP version: ________   System: ________
- Mac RAM and System Folder backup made (yes/no): ____
- Test host address and how it is connected (wired/Wi-Fi): ________
- Router address (ping target): ________

**Transfer format**

- Which format worked with type, creator and icons intact: .bin ____  .hqx ____  both ____  neither ____
- Any file that arrived as a plain document or would not open: ________

**Problems**

- Bomb or MacsBug messages (exact text, and attach a photo): ________
- Step number where each problem happened: ________
- Hangs, slow-downs, mouse or clock glitches noticed: ________
- Photos attached (list): ________

**Measured numbers** (the Try steps say where each one comes from)

| Measure | Value | Unit |
|---|---|---|
| Ping RTT to router, min / avg / max | ____ / ____ / ____ | ms |
| TcpBench connect time | ____ | ms |
| TcpBench echo RTT (17007), avg / max | ____ / ____ | ms |
| Keystroke echo feel in Telnet/Flynn (1 = lag, 5 = instant) | ____ | 1-5 |
| TcpBench bulk receive (17030) | ____ | KB/s |
| TcpBench bulk send (17031) | ____ | KB/s |
| CpuMeter idle, loops/s | ____ | loops/s |
| CpuMeter during bulk receive | ____ | loops/s |
| CpuMeter during bulk send | ____ | loops/s |
| EnetBench: empty READ cost | ____ | us |
| EnetBench: frames/s received | ____ | frames/s |
| EnetBench: frames/s sent | ____ | frames/s |
| EnetBench: echo RTT | ____ | ms |
| EnetBench: polled read 1514-byte frame | ____ | us |
| EnetBench: blind read 1514-byte frame | ____ | us |
| EnetBench: polled write 1514-byte frame | ____ | us |
| ScsiProbe: blind write 1514-byte frame | ____ | us |
| ScsiProbe: did blind transfers bus-error (yes/no, when) | ____ | |
| Poll-rate setting in use (hw2 and later) | ____ | |

**Memory** (system heap, bytes; second run of each; zone size shown by HeapInfo)

| Boot / state | Zone size | FreeMem | MaxMem |
|---|---|---|---|
| MacTCP installed, driver not opened (HeapInfo) | ____ | ____ | ____ |
| MacTCP installed, after HeapInfoOpen of .ENET0 (HeapInfo) | ____ | ____ | ____ |
| HeapInfoOpen shown: FreeMem before / after; app heap cost | ____ / ____ | ____ | |
| MacTCP active, idle (HeapInfo) | ____ | ____ | ____ |
| MacTCP active, after opening MacTCP control panel (HeapInfo) | ____ | ____ | ____ |
| MoofPort INIT installed, driver not opened (hw2 and later) | ____ | ____ | ____ |
| About the Macintosh: Total / System / Finder or Largest Unused (K) | ____ / ____ / ____ | | |

**SpyENET capture**

- `SpyENET.log` copied back (yes/no): ____
- Its last `ring:` line, total events and drops: ________

**Anything else**

- Notes: ________

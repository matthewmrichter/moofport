/* spyenet.r - the SpyENET extension file: an INIT that installs a logging pass-through
 * ".ENET0" (spyenet_init.c), plus the spy DRVR itself (spy_drv.c + spy_drv_stubs.s).
 * File type INIT, creator MPSY. Same code-resource recipe as mac/hello_init and
 * MoofTCP's mac/moof/moof.r (flat Retro68 code per resource, wrapped by Rez).
 *
 *   INIT 128   the installer, run at boot
 *   DRVR 128   the spy driver, name ".ENET0" (header + entry stubs here, code from SpyDrv.flt)
 *   sysz 0     system-heap bytes INIT 31 adds for us (the driver with its log ring)
 *
 * DRVR header + name + stubs layout (offsets in the resource):
 *    0  flags (2)   0x4F00 = dNeedLock | dStatEnable | dCtlEnable | dWritEnable | dReadEnable
 *    2  drvrDelay (2) 0
 *    4  drvrEMask (2) 0
 *    6  drvrMenu  (2) 0
 *    8  open  offset  = 0x001A (26)
 *   10  prime offset  = 0x001E (30)
 *   12  ctl   offset  = 0x0022 (34)
 *   14  stat  offset  = 0x0026 (38)
 *   16  close offset  = 0x002A (42)
 *   18  name "\p.ENET0" (7 bytes: 06 2E 45 4E 45 54 30), then one pad byte at 25
 *   26  open  stub: moveq #0,d0 ; bra.s 50
 *   30  prime stub: moveq #1,d0 ; bra.s 50
 *   34  ctl   stub: moveq #2,d0 ; bra.s 50
 *   38  stat  stub: moveq #3,d0 ; bra.s 50
 *   42  close stub: moveq #4,d0 ; bra.s 50
 *   46  boot  stub: moveq #5,d0 ; nop (falls into 50)   <- INIT calls this to relocate+open
 *   50  flat Retro68 code (its first instruction is the entry trampoline -> _start)
 */
#include "Retro68.r"
#include "MacTypes.r"

type 'INIT' {
    RETRO68_CODE_TYPE
};

resource 'INIT' (128, sysheap, locked) {
    dontBreakAtEntry, $$read("SpyInit.flt");
};

type 'DRVR' {
    hex string;     /* header + name + stubs */
    hex string;     /* flat code */
};

resource 'DRVR' (128, ".ENET0", sysheap, locked) {
    $"4F00 0000 0000 0000"          /* flags, delay, emask, menu */
    $"001A 001E 0022 0026 002A"     /* open, prime, control, status, close offsets */
    $"06 2E 45 4E 45 54 30 00"      /* "\p.ENET0" then 1 pad byte (offset 25) */
    $"7000 6014"                    /* 26 open:    moveq #0,d0; bra.s 50 (disp 0x14) */
    $"7001 6010"                    /* 30 prime:   moveq #1,d0; bra.s 50 (disp 0x10) */
    $"7002 600C"                    /* 34 control: moveq #2,d0; bra.s 50 (disp 0x0C) */
    $"7003 6008"                    /* 38 status:  moveq #3,d0; bra.s 50 (disp 0x08) */
    $"7004 6004"                    /* 42 close:   moveq #4,d0; bra.s 50 (disp 0x04) */
    $"7005 4E71",                   /* 46 boot:    moveq #5,d0; nop (falls into 50) */
    $$read("SpyDrv.flt")
};

data 'sysz' (0) {
    $"0001 0000"
};

resource 'vers' (1) {
    0x00, 0x10, development, 0x00, verUS,
    "0.1d0",
    "SpyENET 0.1d0 (Phase 0 .ENET0 spy)"
};

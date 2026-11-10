/* moofport.r - the MoofPort extension file (type INIT, creator MPRT): the INIT that installs the
 * driver (mp_init.c) and the driver itself (mp_drv.c, mp_scsi.c, the core, mp_drv_stubs.s,
 * mp_drv_ph.s). Same code-resource recipe as SpyENET and MoofTCP's moof.r.
 *
 * DRVR header + name + stubs (offsets in the resource):
 *    0  flags 0x4C00 = dNeedLock (0x4000) | dStatEnable (0x0800) | dCtlEnable (0x0400)
 *    8  open, prime, control, status, close offsets 26, 30, 34, 38, 42
 *   18  name "\p.ENET0" (7 bytes), pad byte at 25
 *   26  open, prime, control, status, close stubs: moveq #n,d0 ; bra.s 50
 *   46  boot stub: moveq #5,d0 ; nop (falls into 50)
 *   50  flat Retro68 code (entry trampoline -> _start in mp_drv_stubs.s)
 */
#include "Retro68.r"
#include "MacTypes.r"

type 'INIT' {
    RETRO68_CODE_TYPE
};

resource 'INIT' (128, sysheap, locked) {
    dontBreakAtEntry, $$read("MoofPortInit.flt");
};

#include "mp_drvr.r"

/* System-heap bytes INIT 31 adds for us: the driver code and the state block, with room. */
data 'sysz' (0) {
    $"0000 8000"
};

resource 'vers' (1) {
    0x01, 0x00, release, 0x00, verUS,
    "1.0",
    "MoofPort 1.0"
};

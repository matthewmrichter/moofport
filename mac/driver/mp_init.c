/* mp_init.c - the MoofPort INIT: installs the MoofPort driver as ".ENET0" at boot.
 *
 *  1. Shift held: do nothing (the usual "skip extension" escape).
 *  2. A ".ENET0" already open in the unit table (AppleTalk opens DaynaPORT at boot when
 *     EtherTalk is selected, before extension INITs run): do nothing, so the two drivers never
 *     poll the device at the same time. The Mac then simply keeps DaynaPORT.
 *  3. Load DRVR 128 (".ENET0") from this file, detach and lock it, and call its boot stub in the
 *     system zone: it relocates the flat code in place and allocates the resident state.
 *  4. Install it as a pointer-based driver in a free unit-table entry, growing the table if needed
 *     (MoofTCP's mt_mac_unit_install). The unit table is searched by name before the DRVR
 *     resources, so OpenDriver(".ENET0") reaches MoofPort and DaynaPORT's DRVR 47 stays unused.
 * The device itself is not touched here: the driver's Open finds and enables it.
 */
#include <Resources.h>
#include <Memory.h>
#include <Events.h>
#include <OSUtils.h>
#include <Retro68Runtime.h>
#include <stdint.h>

#pragma GCC diagnostic ignored "-Warray-bounds"

#define DRVR_ID     128
#define FIRST_UNIT  52
#define BOOT_STUB   46

#define UTableBase   (*(uint8_t ***volatile *)0x011C)
#define UnitNtryCnt  (*(volatile int16_t *)0x01D2)
#define LMSysZone    (*(THz *)0x02A6)

enum { DCE_DRIVER = 0, DCE_FLAGS = 4, DCE_REFNUM = 24, DCE_DELAY = 34, DCE_EMASK = 36,
       DCE_MENU = 38, DCE_SIZE = 40 };
enum { DRVR_FLAGS = 0, DRVR_DELAY = 2, DRVR_EMASK = 4, DRVR_MENU = 6, DRVR_NAME = 18 };

typedef long (*BootProc)(void *drvr);

static uint8_t *strip24(void *p) { return (uint8_t *)((uint32_t)(uintptr_t)p & 0x00FFFFFFu); }
static uint8_t lower(uint8_t c) { return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int shift_held(void)
{
    uint8_t km[16];
    GetKeys(km);
    return km[0x38 >> 3] & (1 << (0x38 & 7));
}

/* The DRVR header behind unit i and its DCE, or 0. */
static uint8_t *unit_header(int16_t i, uint8_t **dce_out)
{
    uint8_t **h = UTableBase[i], *dce, *drv;
    if (!h || !*h) return 0;
    dce = strip24(*h);
    drv = strip24(*(void **)dce);
    if (rd16(dce + DCE_FLAGS) & 0x0040) {           /* dRAMBased: a handle */
        if (!drv || !*(void **)drv) return 0;
        drv = strip24(*(void **)drv);
    }
    *dce_out = dce;
    return drv;
}

static int enet0_open(void)
{
    static const uint8_t want[] = ".enet0";
    int16_t i, k;
    for (i = 0; i < UnitNtryCnt; i++) {
        uint8_t *dce, *drv = unit_header(i, &dce), *nm;
        if (!drv) continue;
        nm = drv + DRVR_NAME;
        if (nm[0] != 6) continue;
        for (k = 0; k < 6 && lower(nm[1 + k]) == want[k]; k++) { }
        if (k == 6 && (dce[DCE_FLAGS + 1] & 0x20)) return 1;   /* dOpened */
    }
    return 0;
}

static int grow_units(int16_t want)
{
    int16_t n = UnitNtryCnt, i;
    uint8_t ***nt = (uint8_t ***)NewPtrSysClear((Size)want * 4);
    uint16_t sr;
    if (!nt) return -1;
    __asm__ volatile("move.w %%sr,%0\n\tor.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    for (i = 0; i < n; i++) nt[i] = UTableBase[i];
    UTableBase = nt;
    UnitNtryCnt = want;
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
    return 0;
}

static int install_unit(uint8_t *hdr)
{
    int16_t unit;
    Handle h;
    uint8_t *dce;
    for (unit = FIRST_UNIT; unit < UnitNtryCnt && UTableBase[unit]; unit++) { }
    if (unit >= UnitNtryCnt && grow_units((int16_t)((unit + 16) & ~15))) return -1;
    h = NewHandleSysClear(DCE_SIZE);
    if (!h) return -1;
    HLock(h);
    dce = strip24(*h);
    wr32(dce + DCE_DRIVER, (uint32_t)(uintptr_t)hdr);    /* pointer-based: never moved */
    wr16(dce + DCE_FLAGS, (uint16_t)(rd16(hdr + DRVR_FLAGS) & 0xFF00u));
    wr16(dce + DCE_REFNUM, (uint16_t)~unit);
    wr16(dce + DCE_DELAY, rd16(hdr + DRVR_DELAY));
    wr16(dce + DCE_EMASK, rd16(hdr + DRVR_EMASK));
    wr16(dce + DCE_MENU, rd16(hdr + DRVR_MENU));
    UTableBase[unit] = (uint8_t **)h;
    return 0;
}

void _start(void);
void _start(void)
{
    Handle h;
    RETRO68_RELOCATE();
    if (!shift_held() && !enet0_open() && (h = Get1Resource('DRVR', DRVR_ID)) != 0) {
        uint8_t *hdr;
        long r;
        THz z = GetZone();
        DetachResource(h);
        HNoPurge(h);
        HLock(h);
        hdr = strip24(*h);
        SetZone(LMSysZone);        /* relocation and the state block go in the system heap */
        r = ((BootProc)(hdr + BOOT_STUB))(hdr);
        SetZone(z);
        if (r != 0 || install_unit(hdr) != 0)
            DisposeHandle(h);       /* the state block (if any) stays: a few KB, boot only */
    }
    Retro68FreeGlobals();
}

/* spyenet_init.c - the SpyENET INIT: at boot it installs a logging pass-through ".ENET0".
 *
 *  1. If Shift is held, do nothing (the usual "skip extension" escape).
 *  2. Load OUR spy driver DRVR 128 (".ENET0") from this file, detach and lock it.
 *  3. Call its boot stub once, which relocates the flat Retro68 block in place.
 *  4. Install it as a pointer-based driver in a free unit-table entry (same recipe as
 *     MoofTCP's mt_mac_unit_install), growing the table if it has no free entry from unit 52
 *     (Snow's table has 64 entries; a real Mac's may have 48). The unit table is searched by
 *     name before the DRVR resources, so MacTCP's OpenDriver(".ENET0") now reaches the spy.
 *  5. If the real driver is already in the unit table as ".ENET0" (AppleTalk opens it at boot
 *     when EtherTalk is selected, before extension INITs run), rename that copy to ".DNET0" so
 *     the name search reaches the spy; the spy then forwards to that open driver.
 *
 * The real DaynaPORT driver is NOT touched here. It cannot be opened at INIT time (MoofTCP
 * MACTCP-INTERFACE.md: its poll reads through CurStackBase, which is -1 until the first
 * application runs, and takes an address error). The spy opens it on its own first open.
 *
 * The INIT's own code is transient; only the detached DRVR block and its DCE stay resident.
 */
#include <Resources.h>
#include <Devices.h>
#include <Memory.h>
#include <Events.h>
#include <OSUtils.h>
#include <Retro68Runtime.h>
#include <stdint.h>

#pragma GCC diagnostic ignored "-Warray-bounds"

#define SPY_DRVR_ID   128        /* our spy DRVR resource id (in this file) */
#define FIRST_UNIT    52         /* above MacTCP's unit 48, inside the 64-entry table */
#define BOOT_STUB     46         /* see spyenet.r */

/* low-memory globals */
#define UTableBase   (*(uint8_t ***volatile *)0x011C)   /* unit table base: DCE handles */
#define UnitNtryCnt  (*(volatile int16_t *)0x01D2)      /* number of unit-table entries */
#define LMSysZone    (*(THz *)0x02A6)

/* DCE and DRVR header offsets (Inside Macintosh II, Device Manager) */
enum { DCE_DRIVER = 0, DCE_FLAGS = 4, DCE_REFNUM = 24, DCE_DELAY = 34, DCE_EMASK = 36,
       DCE_MENU = 38, DCE_SIZE = 40 };
enum { DRVR_FLAGS = 0, DRVR_DELAY = 2, DRVR_EMASK = 4, DRVR_MENU = 6 };

static uint8_t *strip24(void *p) { return (uint8_t *)((uint32_t)(uintptr_t)p & 0x00FFFFFFu); }
static uint8_t lower(uint8_t c) { return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* Grow the unit table to `want` entries: a new block in the system heap, the old one left
 * alone (it may not be a heap block of its own). As MoofTCP's grow_units. */
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

/* The DRVR header behind unit `i`, or 0 (handle-based when the Device Manager loaded it). */
static uint8_t *unit_header(int16_t i)
{
    uint8_t **h = UTableBase[i], *dce, *drv;
    if (!h || !*h) return 0;
    dce = strip24(*h);
    drv = strip24(*(void **)dce);
    if (((dce[DCE_FLAGS] << 8) | dce[DCE_FLAGS + 1]) & 0x0040) {   /* dRAMBased: a handle */
        if (!drv || !*(void **)drv) return 0;
        drv = strip24(*(void **)drv);
    }
    return drv;
}

/* Rename every other ".ENET0" in the unit table to ".DNET0" (same length, in place). */
static void rename_other_enet0(const uint8_t *ours)
{
    static const uint8_t want[] = ".enet0";
    int16_t i, k;
    for (i = 0; i < UnitNtryCnt; i++) {
        uint8_t *drv = unit_header(i), *nm;
        if (!drv || drv == ours) continue;
        nm = drv + 18;
        if (nm[0] != 6) continue;
        for (k = 0; k < 6 && lower(nm[1 + k]) == want[k]; k++) { }
        if (k == 6) nm[2] = 'D';
    }
}

/* Install the driver at `hdr` in the first free unit from FIRST_UNIT; 0 on success. */
static int install_unit(uint8_t *hdr)
{
    int16_t unit;
    Handle h;
    uint8_t *dce;
    for (unit = FIRST_UNIT; unit < UnitNtryCnt && UTableBase[unit]; unit++) { }
    if (unit >= UnitNtryCnt) {
        if (grow_units((int16_t)((unit + 16) & ~15))) return -1;
    }
    h = NewHandleSysClear(DCE_SIZE);
    if (!h) return -1;
    HLock(h);
    dce = strip24(*h);
    wr32(dce + DCE_DRIVER, (uint32_t)(uintptr_t)hdr);    /* pointer-based: never locked/moved */
    wr16(dce + DCE_FLAGS, (uint16_t)(rd16(hdr + DRVR_FLAGS) & 0xFF00u));
    wr16(dce + DCE_REFNUM, (uint16_t)~unit);
    wr16(dce + DCE_DELAY, rd16(hdr + DRVR_DELAY));
    wr16(dce + DCE_EMASK, rd16(hdr + DRVR_EMASK));
    wr16(dce + DCE_MENU, rd16(hdr + DRVR_MENU));
    UTableBase[unit] = (uint8_t **)h;
    return 0;
}

static int shift_held(void)
{
    uint8_t km[16];
    GetKeys(km);
    return km[0x38 >> 3] & (1 << (0x38 & 7));   /* Shift is key code 0x38 */
}

void _start(void);
void _start(void)
{
    Handle spyH;

    RETRO68_RELOCATE();

    if (!shift_held()) {
        spyH = Get1Resource('DRVR', SPY_DRVR_ID);
        if (spyH) {
            uint8_t *hdr;
            void (*spyBoot)(void);
            THz z = GetZone();
            DetachResource(spyH);
            HNoPurge(spyH);
            HLock(spyH);
            hdr = strip24(*spyH);
            spyBoot = (void (*)(void))(hdr + BOOT_STUB);
            SetZone(LMSysZone);     /* relocation allocates the block's BSS in the current zone */
            spyBoot();
            SetZone(z);
            if (install_unit(hdr) != 0) DisposeHandle(spyH);
            else rename_other_enet0(hdr);
        }
    }
    Retro68FreeGlobals();
}

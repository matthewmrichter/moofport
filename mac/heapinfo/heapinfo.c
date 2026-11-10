/* heapinfo.c - HeapInfo / HeapInfoOpen: system-heap figures for the driver memory cap.
 *
 * Port of MoofTCP's mac/heapinfo (docs/dev/EMULATOR.md "MacTCP memory baseline"), reworked
 * after hardware session 1 (2026-11-06). FreeMem alone is not a footprint on a real Mac:
 * MaxMem purges purgeable blocks, and under MultiFinder the system heap grows on demand, so a
 * reading taken after a purge or a growth step moves for reasons that have nothing to do with
 * the driver (the hw1 HeapInfoOpen run reported a cost of -20,808 bytes). So these tools walk
 * the heap and sum its blocks by kind:
 *
 *   fixed      nonrelocatable blocks, plus relocatable blocks that are not purgeable
 *   purgeable  relocatable blocks marked purgeable (and not locked)
 *   free       free blocks
 *
 * "fixed" is what nothing can take back; a driver's footprint is the change in "fixed". The walk
 * neither allocates nor purges. FreeMem, MaxMem and the zone size are still printed for
 * comparison with earlier runs; MaxMem (which purges) is read last.
 *
 *   HeapInfo      (creator MPHP): one reading.
 *   HeapInfoOpen  (creator MPHQ, built with -DOPEN_ENET): a reading, OpenDriver(".ENET0"), a
 *                 reading, and the differences, for the system heap and the application heap.
 *
 * Heap layout (Inside Macintosh II, 24-bit Memory Manager, all System 6 Macs with a 68000): the
 * blocks run from the zone's heapData to bkLim. Each has an 8-byte header: byte 0 is the tag
 * (bits 7-6: 00 free, 01 nonrelocatable, 10 relocatable), bytes 1-3 the physical size in bytes
 * including the header; for a relocatable block the next long is the master pointer's offset
 * from the zone start, and the master pointer's high byte holds the flags (0x80 locked, 0x40
 * purgeable). The SysZone global (0x2A6) gives the system zone.
 *
 * Results go to an alert and to HeapInfo-<run id>.out (or HeapInfoOpen-...) next to the app.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Memory.h>
#include <Files.h>
#include <Devices.h>
#include <OSUtils.h>
#include <stdio.h>
#include <string.h>
#include "runid.h"

QDGlobals qd;

#ifdef OPEN_ENET
#define APPTITLE "HeapInfoOpen"
#else
#define APPTITLE "HeapInfo"
#endif

/* Append a C string to the open output file (ignore errors: the alert still shows). */
static short g_outref = -1;
static Str255 g_outname;      /* "<Tool>-<run id>.out", see runid.c */
static void wr(const char *s)
{
    long n = (long)strlen(s);
    if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s);
}

typedef struct Heap {
    long fixed, purgeable, free;    /* bytes, block headers included */
    long blocks, zone, freeMem;     /* zone = bkLim - zone start */
    int bad;                        /* the walk met a block it could not read */
} Heap;

static unsigned long rd32(const unsigned char *p)
{ return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3]; }

/* Walk `z` without allocating or purging. FreeMem is read in the same zone, also harmless. */
static void walk(THz z, Heap *h)
{
    THz saved = GetZone();
    unsigned long base = (unsigned long)z & 0x00FFFFFFUL;
    unsigned char *p = (unsigned char *)&z->heapData;
    unsigned char *lim = (unsigned char *)((unsigned long)z->bkLim & 0x00FFFFFFUL);
    memset(h, 0, sizeof *h);
    h->zone = (long)(lim - (unsigned char *)base);
    while (p < lim) {
        unsigned long size = rd32(p) & 0x00FFFFFFUL;
        unsigned tag = p[0] >> 6;
        if (size < 12 || p + size > lim) { h->bad = 1; break; }
        if (tag == 0) {
            h->free += (long)size;
        } else if (tag == 1) {
            h->fixed += (long)size;
        } else if (tag == 2) {
            unsigned char *mp = (unsigned char *)(base + rd32(p + 4));
            unsigned char flags = mp[0];
            if ((flags & 0x40) && !(flags & 0x80)) h->purgeable += (long)size;
            else h->fixed += (long)size;
        } else {
            h->bad = 1;
            break;
        }
        h->blocks++;
        p += size;
    }
    SetZone(z);
    h->freeMem = FreeMem();
    SetZone(saved);
}

static THz sys_zone(void) { return *(THz *)0x2A6; }   /* low-memory SysZone */

static long max_mem(THz z)          /* MaxMem purges: call it only after the walks */
{
    THz saved = GetZone();
    Size grow = 0;
    long m;
    SetZone(z);
    m = MaxMem(&grow);
    SetZone(saved);
    return m;
}

static void put(const char *label, const Heap *h)
{
    char line[200];
    sprintf(line, "%s: fixed %ld  purgeable %ld  free %ld  (zone %ld, FreeMem %ld, %ld blocks%s)\r",
            label, h->fixed, h->purgeable, h->free, h->zone, h->freeMem, h->blocks,
            h->bad ? ", WALK STOPPED EARLY" : "");
    wr(line);
}

int main(void)
{
    char line[256];
    Str255 ps;
    size_t n;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();

    runid_name(APPTITLE, ".out", g_outname);
    FSDelete(g_outname, 0);
    Create(g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen(g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);

    wr(APPTITLE "\r");

#ifdef OPEN_ENET
    {
        short ref = 0;
        OSErr err;
        Heap sb, sa, ab, aa;
        walk(sys_zone(), &sb);
        walk(ApplicationZone(), &ab);
        err = OpenDriver((ConstStr255Param)"\p.ENET0", &ref);
        walk(sys_zone(), &sa);
        walk(ApplicationZone(), &aa);
        sprintf(line, "OpenDriver(.ENET0) = %d  refnum %d\r"
                      "System heap open cost: fixed %+ld, purgeable %+ld, zone %+ld\r"
                      "App heap open cost: fixed %+ld\r",
                (int)err, (int)ref, sa.fixed - sb.fixed, sa.purgeable - sb.purgeable,
                sa.zone - sb.zone, aa.fixed - ab.fixed);
        wr(line);
        put("system before", &sb);
        put("system after", &sa);
        put("app before", &ab);
        put("app after", &aa);
        sprintf(line, "system MaxMem after (purges) %ld\r", max_mem(sys_zone()));
        wr(line);
    }
#else
    {
        Heap s0;
        walk(sys_zone(), &s0);
        sprintf(line, "System heap: fixed %ld, purgeable %ld, free %ld, zone %ld\r",
                s0.fixed, s0.purgeable, s0.free, s0.zone);
        wr(line);
        put("system", &s0);
        sprintf(line, "system MaxMem (purges) %ld, FreeMem after it %ld\r", max_mem(sys_zone()),
                (walk(sys_zone(), &s0), s0.freeMem));
        wr(line);
    }
#endif

    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);

    /* Show the whole file text in the alert (fits for these short reports). */
    {
        short r2 = -1;
        long len = 0;
        if (FSOpen(g_outname, 0, &r2) == noErr) {
            GetEOF(r2, &len);
            if (len > 250) len = 250;
            FSRead(r2, &len, (Ptr)line);
            FSClose(r2);
            line[len] = 0;
        } else {
            strcpy(line, APPTITLE ": could not re-read output file");
        }
    }
    n = strlen(line);
    if (n > 255) n = 255;
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, line, n);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

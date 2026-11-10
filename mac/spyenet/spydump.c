/* spydump.c - SpyDump: read SpyENET's in-RAM log ring and append it as text.
 *
 * It opens ".ENET0" (which, with SpyENET installed, is the spy), issues a PRIVATE Status call
 * (csCode SPY_PRIV_RING) that returns the ring pointer in the PB's ePointer field, then walks
 * the ring, APPENDS a readable line per entry to "SpyENET-<run id>.log" next to the app, and empties
 * the ring so the next run of SpyDump continues the capture. If the returned pointer's magic
 * is not ours it reports that SpyENET is not installed.
 *
 * The SpyRing / SpyEntry layout here MUST match spy_drv.c.
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
#include <stdint.h>

QDGlobals qd;

#define SPY_PRIV_RING 0x1F00
#define SPY_MAGIC   0x53505954u
#define SPY_SLOTS   1024
enum { K_OPEN = 0, K_PRIME, K_CONTROL, K_STATUS, K_CLOSE, K_HANDLER, K_SCSI };

#pragma pack(push, 2)
typedef struct EPB {
    uint32_t qLink; int16_t qType; int16_t ioTrap; uint32_t ioCmdAddr;
    uint32_t ioCompletion; volatile int16_t ioResult; uint32_t ioNamePtr;
    int16_t ioVRefNum; int16_t ioRefNum; int16_t csCode; int16_t eProtType;
    uint32_t ePointer; int16_t eBuffSize; int16_t eDataSize; int16_t spare[10];
} EPB;
typedef struct SpyEntry {
    uint32_t tick; uint16_t kind; uint16_t csCode; uint16_t ioTrap; uint16_t sr;
    uint16_t eProtType; uint16_t etherType; uint16_t wdsPieces; uint16_t len0;
    uint16_t totalLen; int16_t result; uint16_t len1; uint16_t depth; uint32_t ioCompletion;
} SpyEntry;
typedef struct SpyCounts {
    uint32_t cs[9]; uint32_t csOther; uint32_t handler;
    uint32_t ewriteAsync, ewriteImmed, ewriteMasked, ewriteInPH, ewriteNoSlot;
    uint16_t ewriteMaxPieces, ewriteMaxLen, ewriteMinLen, srOr;
} SpyCounts;
typedef struct SpyRing {
    uint32_t magic; uint16_t head, tail; uint16_t nslots; uint16_t drops;
    uint32_t total; SpyCounts n; SpyEntry e[SPY_SLOTS];
} SpyRing;
#pragma pack(pop)

static short g_outref = -1;
static Str255 g_outname;      /* "<Tool>-<run id>.out", see runid.c */
static char g_line[256];
static void wr(const char *s) { long n = (long)strlen(s); if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s); }
#define LOG(...) do { sprintf(g_line, __VA_ARGS__); wr(g_line); } while (0)

static const char *kindname(uint16_t k)
{
    switch (k) {
        case K_OPEN: return "open";
        case K_PRIME: return "prime";
        case K_CONTROL: return "control";
        case K_STATUS: return "status";
        case K_CLOSE: return "close";
        case K_HANDLER: return "handler";
        default: return "?";
    }
}

static const char *cscode_name(uint16_t cs)
{
    switch (cs) {
        case 245: return "EDelMulti";      /* ENET.h: 245 ENetDelMulti, 246 ENetAddMulti */
        case 246: return "EAddMulti";
        case 247: return "EAttachPH";
        case 248: return "EDetachPH";
        case 249: return "EWrite";
        case 250: return "ERead";
        case 251: return "ERdCancel";
        case 252: return "EGetInfo";
        case 253: return "ESetGeneral";
        default: return "";
    }
}

int main(void)
{
    EPB pb;
    SpyRing *ring;
    short ref = 0;
    int16_t r;
    uint16_t i, idx, n;
    Str255 ps;
    size_t ln;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
    runid_name("SpyENET", ".log", g_outname);   /* the log is named after the spy */

    Create((ConstStr255Param)g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStr255Param)g_outname, 0, &g_outref)) g_outref = -1;
    else SetFPos(g_outref, fsFromLEOF, 0);
    wr("---- SpyDump\r");

    r = (int16_t)OpenDriver((ConstStr255Param)"\p.ENET0", &ref);
    if (r) { LOG("OpenDriver(.ENET0) failed: %d (SpyENET not installed?)\r", (int)r); goto fin; }

    memset(&pb, 0, sizeof pb);
    pb.ioRefNum = ref;
    pb.csCode = SPY_PRIV_RING;
    pb.ioCompletion = 0;
    r = (int16_t)PBStatusSync((ParmBlkPtr)&pb);
    if (r) { LOG("private Status (ring) failed: %d\r", (int)r); goto fin; }
    ring = (SpyRing *)(uintptr_t)pb.ePointer;
    if (!ring || ring->magic != SPY_MAGIC) {
        LOG("no SpyENET ring (magic mismatch): .ENET0 is not the spy\r");
        goto fin;
    }

    LOG("ring: total events %lu, drops %u, slots %u\r",
        (unsigned long)ring->total, (unsigned)ring->drops, (unsigned)ring->nslots);
    for (i = 0; i < 9; i++)
        if (ring->n.cs[i])
            LOG("count: csCode %u %s = %lu\r", (unsigned)(245 + i), cscode_name((uint16_t)(245 + i)),
                (unsigned long)ring->n.cs[i]);
    LOG("count: other csCodes %lu, handler deliveries %lu, SR OR 0x%04x\r",
        (unsigned long)ring->n.csOther, (unsigned long)ring->n.handler, (unsigned)ring->n.srOr);
    LOG("count: EWrite async %lu, immediate %lu, under mask %lu, inside handler %lu, no slot %lu\r",
        (unsigned long)ring->n.ewriteAsync, (unsigned long)ring->n.ewriteImmed,
        (unsigned long)ring->n.ewriteMasked, (unsigned long)ring->n.ewriteInPH,
        (unsigned long)ring->n.ewriteNoSlot);
    LOG("count: EWrite max pieces %u, frame length min %u max %u\r",
        (unsigned)ring->n.ewriteMaxPieces, (unsigned)ring->n.ewriteMinLen,
        (unsigned)ring->n.ewriteMaxLen);
    /* walk from tail to head (oldest to newest currently held) */
    idx = ring->tail;
    n = 0;
    while (idx != ring->head) {
        SpyEntry *e = &ring->e[idx];
        if (e->kind == K_SCSI) {
            LOG("%8lu  scsi     cmd %02x %02x %02x %02x %02x %02x (len %u)  SR=0x%04x\r",
                (unsigned long)e->tick, (unsigned)(e->eProtType >> 8), (unsigned)(e->eProtType & 0xFF),
                (unsigned)(e->etherType >> 8), (unsigned)(e->etherType & 0xFF),
                (unsigned)(e->len0 >> 8), (unsigned)(e->len0 & 0xFF), (unsigned)e->len1, (unsigned)e->sr);
        } else if (e->kind == K_HANDLER) {
            LOG("%8lu  handler  etherType=0x%04x  SR=0x%04x  D1(len-14+4)=%u depth=%u\r",
                (unsigned long)e->tick, (unsigned)e->etherType, (unsigned)e->sr, (unsigned)e->len0,
                (unsigned)e->depth);
        } else if (e->kind == K_CONTROL && e->csCode == 249) {
            LOG("%8lu  control  EWrite  trap=0x%04x SR=0x%04x compl=%08lx depth=%u  WDS pieces=%u first=%u second=%u total=%u\r",
                (unsigned long)e->tick, (unsigned)e->ioTrap, (unsigned)e->sr,
                (unsigned long)e->ioCompletion, (unsigned)e->depth,
                (unsigned)e->wdsPieces, (unsigned)e->len0, (unsigned)e->len1, (unsigned)e->totalLen);
        } else if (e->kind == K_CONTROL && (e->csCode == 247 || e->csCode == 248)) {
            LOG("%8lu  control  %s  eProtType=0x%04x  trap=0x%04x SR=0x%04x  res=%d\r",
                (unsigned long)e->tick, cscode_name(e->csCode), (unsigned)e->eProtType,
                (unsigned)e->ioTrap, (unsigned)e->sr, (int)e->result);
        } else {
            LOG("%8lu  %-7s  csCode=%u %s  trap=0x%04x SR=0x%04x compl=%08lx  eBuffSize=%u eDataSize=%u  res=%d\r",
                (unsigned long)e->tick, kindname(e->kind), (unsigned)e->csCode,
                cscode_name(e->csCode), (unsigned)e->ioTrap, (unsigned)e->sr,
                (unsigned long)e->ioCompletion, (unsigned)e->len0, (unsigned)e->len1, (int)e->result);
        }
        idx = (uint16_t)((idx + 1) % SPY_SLOTS);
        n++;
    }
    ring->tail = idx;           /* empty the ring: the next SpyDump continues from here */
    ring->drops = 0;
    LOG("wrote %u entries\r", (unsigned)n);

fin:
    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);
    sprintf(g_line, "SpyDump finished. See %.*s next to SpyDump.", (int)g_outname[0], (const char *)g_outname + 1);
    ln = strlen(g_line);
    ps[0] = (unsigned char)ln;
    memcpy(ps + 1, g_line, ln);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

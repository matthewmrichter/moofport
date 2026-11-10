/* spy_drv.c - the SpyENET pass-through .ENET0 driver: logs every call MacTCP makes and
 * forwards it to the real DaynaPORT driver. Built as a flat Retro68 code block wrapped as a
 * DRVR resource named ".ENET0" (spyenet.r). See spy_drv_stubs.s and README.md.
 *
 * ALL state lives in THIS block's globals (the block is detached + locked in the system heap
 * by the INIT, so it is resident; Retro68 --mac-flat addresses globals absolutely after
 * relocation, with no A5 world, so a driver may use them). Nothing is allocated at interrupt
 * time. The log ring is a global; SpyDump reads it through a private Status csCode.
 *
 * The real driver: the first spy_open hides our own name, lets the Device Manager load and
 * open DRVR 47 the normal way (unit 47, refnum -48, as without the spy), renames the loaded
 * copy to ".DNET0" and restores our name, so ".ENET0" keeps meaning the spy. It cannot happen
 * earlier: DaynaPORT must not be opened at INIT time (README.md).
 *
 * Forwarding model (see README.md):
 *  - EWrite (async in MacTCP, and must stay async: a sync EWrite from a handler / TM task /
 *    masked context HANGS, MoofTCP docs/dev/MACTCP-INTERFACE.md) is forwarded on a COPIED PB
 *    issued PBControlAsync to the real driver, with our completion spy_fwd_complete, which
 *    copies the result back and jIODone's the ORIGINAL. spy_control returns 1 = "I will
 *    complete it later", so the Device Manager leaves the original pending (works for both a
 *    sync and an async caller: a sync caller just spins on ioResult until our completion).
 *  - every other Control (EAttachPH/EDetachPH/EGetInfo/ESetGeneral/EAddMulti/EDelMulti) is
 *    forwarded SYNCHRONOUSLY on a stack copy (these never hang) and the result returned.
 *  - EAttachPH is additionally WRAPPED: MacTCP's handler is saved and our spy_ph is attached
 *    to the real driver in its place; spy_ph logs each delivery then chains to MacTCP's.
 */
#include <Devices.h>
#include <Memory.h>
#include <Resources.h>
#include <OSUtils.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#pragma GCC diagnostic ignored "-Warray-bounds"

enum { E_ATTACH = 247, E_DETACH = 248, E_WRITE = 249, E_GETINFO = 252, E_SETGENERAL = 253 };
#define SPY_PRIV_RING 0x1F00    /* private Status csCode: return &g_ring in ePointer */

/* ---- Ethernet PB (csParam at 28; size 58), as elsewhere in MoofPort ---- */
#pragma pack(push, 2)
typedef struct EPB {
    uint32_t qLink;
    int16_t  qType;
    int16_t  ioTrap;
    uint32_t ioCmdAddr;
    uint32_t ioCompletion;
    volatile int16_t ioResult;
    uint32_t ioNamePtr;
    int16_t  ioVRefNum;
    int16_t  ioRefNum;
    int16_t  csCode;
    int16_t  eProtType;
    uint32_t ePointer;
    int16_t  eBuffSize;
    int16_t  eDataSize;
    int16_t  spare[10];
} EPB;
#pragma pack(pop)

/* ---- the log ring (a resident global); spydump.c mirrors this layout ---- */
#define SPY_MAGIC   0x53505954u     /* 'SPYT' (layout version) */
#define SPY_SLOTS   1024
enum { K_OPEN = 0, K_PRIME, K_CONTROL, K_STATUS, K_CLOSE, K_HANDLER, K_SCSI };

typedef struct SpyEntry {
    uint32_t tick;        /* TickCount at entry */
    uint16_t kind;        /* K_* */
    uint16_t csCode;
    uint16_t ioTrap;      /* raw: bit 0x0400 async, 0x0200 immediate */
    uint16_t sr;          /* SR at entry */
    uint16_t eProtType;   /* for attach/detach */
    uint16_t etherType;   /* for handler deliveries */
    uint16_t wdsPieces;   /* EWrite: WDS piece count */
    uint16_t len0;        /* EWrite: first WDS piece length; handler: D1 bytes-left; else eBuffSize */
    uint16_t totalLen;    /* EWrite: sum of WDS lengths (frame length) */
    int16_t  result;      /* result of a sync forward (handler, EWrite: 0) */
    uint16_t len1;        /* EWrite: second WDS piece length; else eDataSize */
    uint16_t depth;       /* protocol-handler nesting when this entry was logged */
    uint32_t ioCompletion;
} SpyEntry;

/* Every event is counted here even when the ring is full. */
typedef struct SpyCounts {
    uint32_t cs[9];         /* Control calls by csCode 245..253 */
    uint32_t csOther;
    uint32_t handler;
    uint32_t ewriteAsync, ewriteImmed, ewriteMasked, ewriteInPH, ewriteNoSlot;
    uint16_t ewriteMaxPieces, ewriteMaxLen, ewriteMinLen, srOr;
} SpyCounts;

typedef struct SpyRing {
    uint32_t magic;
    uint16_t head, tail;
    uint16_t nslots;
    uint16_t drops;
    uint32_t total;       /* total events ever seen (wraps) */
    SpyCounts n;
    SpyEntry e[SPY_SLOTS];
} SpyRing;

static SpyRing g_ring = { SPY_MAGIC, 0, 0, SPY_SLOTS, 0, 0, { { 0 }, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFFFF, 0 }, { { 0 } } };

/* ---- driver state (resident) ---- */
void *spy_our_dce = 0;              /* our DCE (used by spy_fwd_complete asm) */
extern void spy_scsi_patch(void);   /* spy_drv_stubs.s: logs, then jumps to spy_scsi_orig */
short spy_real_ref = 0;            /* the real DaynaPORT driver refnum */
volatile uint16_t spy_ph_depth = 0; /* protocol handlers currently running (spy_ph) */

#define SPY_NHAND 4
static struct { uint16_t type; uint16_t inUse; uint32_t handler; } g_hand[SPY_NHAND];

#define SPY_NFWD 8
typedef struct SpyFwd { EPB copy; void *orig; uint8_t used; uint8_t pad; } SpyFwd;
static SpyFwd g_fwd[SPY_NFWD];

extern void spy_ph(void);           /* the wrapped protocol handler (asm) */
extern void spy_fwd_complete(void); /* async-forward completion (asm) */

/* ---- helpers ---- */
static uint16_t get_sr(void)
{
    uint16_t s;
    __asm__ volatile("move.w %%sr,%0" : "=d"(s));
    return s;
}
static uint32_t ticks(void) { return *(volatile uint32_t *)0x16A; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Task and interrupt contexts both log, so each log operation runs with interrupts off. */
static inline uint16_t irq_off(void)
{
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0\n\tor.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    return sr;
}
static inline void irq_on(uint16_t sr) { __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc"); }

static SpyEntry *ring_next(void)
{
    uint16_t h = g_ring.head;
    uint16_t n = (uint16_t)((h + 1) % SPY_SLOTS);
    SpyEntry *e;
    g_ring.total++;
    if (n == g_ring.tail) { g_ring.drops++; return 0; }   /* reader behind: drop */
    e = &g_ring.e[h];
    memset(e, 0, sizeof *e);
    e->tick = ticks();
    e->depth = spy_ph_depth;
    g_ring.head = n;
    return e;
}

/* ---- the SCSI Manager trace (hwsend6: DaynaPORT's reply path, behavioural) ----
 * A head patch on _SCSIDispatch (spy_scsi_patch, spy_drv_stubs.s) logs every SCSICmd sent to
 * SPY_SCSI_ID (the last SCSISelect's target) as a K_SCSI entry: csCode = the selector, eProtType
 * / etherType / len0 = CDB bytes 0-1 / 2-3 / 4-5, len1 = the CDB length. It starts at the first
 * EWrite of 500 bytes or more (a bulk send; Snow's NAT segments are 590 bytes), so the ring holds that send, not the idle polls. */
#define SPY_SCSI_ID 3               /* the DaynaPORT in Snow's workspace */
uint32_t spy_scsi_orig = 0;         /* the real _SCSIDispatch (spy_scsi_patch jumps there) */
static uint16_t g_scsi_target = 0xFFFF;
static uint8_t g_scsi_armed;
static SpyEntry *ring_next(void);
static uint16_t irq_off(void);
static void irq_on(uint16_t sr);

void spy_log_scsi(const uint8_t *stk, uint16_t sr);
void spy_log_scsi(const uint8_t *stk, uint16_t sr)
{
    uint16_t sel = rd16(stk + 4), s;
    SpyEntry *e;
    if (sel == 2)                           /* SCSISelect(targetID) */
        g_scsi_target = rd16(stk + 6);
    if (sel != 3 || g_scsi_target != SPY_SCSI_ID || !g_scsi_armed)
        return;
    s = irq_off();
    e = ring_next();
    if (e) {                                /* SCSICmd(buffer, count): count at 6, buffer at 8 */
        const uint8_t *cdb = (const uint8_t *)(uintptr_t)((uint32_t)rd16(stk + 8) << 16 | rd16(stk + 10));
        e->kind = K_SCSI;
        e->csCode = sel;
        e->sr = sr;
        e->len1 = rd16(stk + 6);
        e->eProtType = (uint16_t)(cdb[0] << 8 | cdb[1]);
        e->etherType = (uint16_t)(cdb[2] << 8 | cdb[3]);
        e->len0 = (uint16_t)(cdb[4] << 8 | cdb[5]);
    }
    irq_on(s);
}

/* parse an EWrite WDS: fill pieces, first length and total length */
static void parse_wds(uint32_t wdsPtr, uint16_t *pieces, uint16_t *len0, uint16_t *len1, uint16_t *total)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)wdsPtr;
    uint16_t pc = 0, tot = 0, first = 0;
    if (p) {
        for (;;) {
            uint16_t len = rd16(p);
            if (len == 0) break;
            if (pc == 0) first = len;
            if (pc == 1) *len1 = len;
            tot = (uint16_t)(tot + len);
            pc++;
            p += 6;                 /* {uint16 len; uint32 ptr} */
            if (pc > 32) break;     /* defensive */
        }
    }
    *pieces = pc; *len0 = first; *total = tot;
}

static SpyEntry *log_call(uint16_t kind, EPB *pb, uint16_t sr)
{
    uint16_t s = irq_off();
    uint16_t pieces = 0, len0 = 0, len1 = 0, total = 0;
    int ewrite = (kind == K_CONTROL && pb->csCode == E_WRITE);
    SpyEntry *e;
    if (ewrite) parse_wds(pb->ePointer, &pieces, &len0, &len1, &total);
    g_ring.n.srOr |= sr;
    if (kind == K_CONTROL) {
        uint16_t cs = (uint16_t)pb->csCode;
        if (cs >= 245 && cs <= 253) g_ring.n.cs[cs - 245]++; else g_ring.n.csOther++;
    }
    if (ewrite) {
        if (total >= 500) g_scsi_armed = 1;
        if ((uint16_t)pb->ioTrap & 0x0400) g_ring.n.ewriteAsync++;
        if ((uint16_t)pb->ioTrap & 0x0200) g_ring.n.ewriteImmed++;
        if (sr & 0x0700) g_ring.n.ewriteMasked++;
        if (spy_ph_depth) g_ring.n.ewriteInPH++;
        if (pieces > g_ring.n.ewriteMaxPieces) g_ring.n.ewriteMaxPieces = pieces;
        if (total > g_ring.n.ewriteMaxLen) g_ring.n.ewriteMaxLen = total;
        if (total < g_ring.n.ewriteMinLen) g_ring.n.ewriteMinLen = total;
    }
    e = ring_next();
    if (e) {
        e->kind = kind;
        e->csCode = (uint16_t)pb->csCode;
        e->ioTrap = (uint16_t)pb->ioTrap;
        e->sr = sr;
        e->eProtType = (uint16_t)pb->eProtType;
        e->ioCompletion = pb->ioCompletion;
        e->wdsPieces = pieces; e->len0 = len0; e->len1 = len1; e->totalLen = total;
        if (!ewrite) { e->len0 = (uint16_t)pb->eBuffSize; e->len1 = (uint16_t)pb->eDataSize; }
    }
    irq_on(s);
    return e;
}

static int16_t log_result(SpyEntry *e, int16_t r)
{
    if (e) e->result = r;
    return r;
}

/* ---- handler table ---- */
static void hand_add(uint16_t type, uint32_t handler)
{
    int i;
    for (i = 0; i < SPY_NHAND; i++)
        if (g_hand[i].inUse && g_hand[i].type == type) { g_hand[i].handler = handler; return; }
    for (i = 0; i < SPY_NHAND; i++)
        if (!g_hand[i].inUse) { g_hand[i].inUse = 1; g_hand[i].type = type; g_hand[i].handler = handler; return; }
}
static void hand_del(uint16_t type)
{
    int i;
    for (i = 0; i < SPY_NHAND; i++)
        if (g_hand[i].inUse && g_hand[i].type == type) g_hand[i].inUse = 0;
}

/* called by spy_ph (asm): return MacTCP's handler for this EtherType, or 0 */
uint32_t spy_find_handler(uint16_t etherType);
uint32_t spy_find_handler(uint16_t etherType)
{
    int i;
    for (i = 0; i < SPY_NHAND; i++)
        if (g_hand[i].inUse && g_hand[i].type == etherType) return g_hand[i].handler;
    return 0;
}

/* called by spy_ph (asm) per frame delivery */
void spy_log_handler(uint16_t etherType, uint16_t sr, uint16_t d1len);
void spy_log_handler(uint16_t etherType, uint16_t sr, uint16_t d1len)
{
    uint16_t s = irq_off();
    SpyEntry *e;
    g_ring.n.handler++;
    g_ring.n.srOr |= sr;
    e = ring_next();
    if (e) {
        e->kind = K_HANDLER;
        e->etherType = etherType;
        e->sr = sr;
        e->len0 = d1len;
    }
    irq_on(s);
}

/* ---- forwarding ---- */
static int16_t forward_sync(EPB *pb, int status)
{
    EPB copy;
    int16_t r;
    copy = *pb;
    copy.ioRefNum = spy_real_ref;
    copy.ioCompletion = 0;
    copy.ioResult = 1;
    r = (int16_t)(status ? PBStatusSync((ParmBlkPtr)&copy) : PBControlSync((ParmBlkPtr)&copy));
    /* copy outputs back (the real driver may have written csParam, e.g. EGetInfo's eDataSize;
     * EGetInfo's data went to the shared buffer at ePointer directly). */
    pb->eProtType = copy.eProtType;
    pb->eBuffSize = copy.eBuffSize;
    pb->eDataSize = copy.eDataSize;
    pb->ioResult = copy.ioResult;
    return r;
}

/* EWrite: async forward on a copied PB; returns 1 ("complete later") or a negative error if
 * the async issue was refused at once (then pb->ioResult is set and we return that error). */
static int16_t forward_ewrite_async(EPB *pb)
{
    int i;
    SpyFwd *s = 0;
    int16_t r;
    for (i = 0; i < SPY_NFWD; i++) if (!g_fwd[i].used) { s = &g_fwd[i]; break; }
    if (!s) { g_ring.n.ewriteNoSlot++; pb->ioResult = -1; return -1; }    /* no free slot: fail the write */
    s->used = 1;
    s->orig = pb;
    s->copy = *pb;
    s->copy.ioRefNum = spy_real_ref;
    s->copy.ioCompletion = (uint32_t)(uintptr_t)spy_fwd_complete;
    s->copy.ioResult = 1;
    r = (int16_t)PBControlAsync((ParmBlkPtr)&s->copy);
    if (r < 0) { s->used = 0; pb->ioResult = r; return r; }   /* refused: complete now with error */
    return 1;                                                 /* accepted: complete later */
}

/* called by spy_fwd_complete (asm) with the copy PB; copy result into orig, free slot,
 * return the original PB pointer. */
void *spy_fwd_done_c(void *copyPB);
void *spy_fwd_done_c(void *copyPB)
{
    SpyFwd *s = (SpyFwd *)((uint8_t *)copyPB - offsetof(SpyFwd, copy));
    EPB *orig = (EPB *)s->orig;
    orig->ioResult = s->copy.ioResult;
    s->used = 0;
    return orig;
}

/* boot: called once by the INIT through the boot stub, after relocation. Nothing to do yet;
 * the real driver must not be opened at INIT time (see spy_real_open). */
void spy_boot(void);
void spy_boot(void) { }

/* ---- the real driver ---- */
#define UTableBase  (*(uint8_t ***volatile *)0x011C)   /* unit table base: DCE handles */
#define LMSysZone   (*(THz *)0x02A6)
#define LMSysMap    (*(volatile short *)0x0A58)
#define DRVR_NAME   18
#define F_RAMBASED  0x0040u

static uint8_t *strip24(const void *p) { return (uint8_t *)((uint32_t)(uintptr_t)p & 0x00FFFFFFu); }

/* The DRVR header behind a refnum (handle-based when the Device Manager loaded it). */
static uint8_t *unit_header(short ref)
{
    uint8_t *dce = strip24(*UTableBase[(short)~ref]);
    uint8_t *drv = strip24(*(void **)dce);
    if (((dce[4] << 8) | dce[5]) & F_RAMBASED) drv = strip24(*(void **)drv);
    return drv;
}

/* Load and open DRVR 47 the normal way, with our own name hidden so the Device Manager does
 * not find us instead, then give the loaded copy a different name. Same zone and resource
 * file as MoofTCP's link_up: the Ethernet driver allocates in the current zone. */
/* The refnum of an open unit whose driver is named ".DNET0": the real driver, already opened at
 * boot (by AppleTalk) and renamed by our INIT. 0 if there is none. */
static short opened_dnet0(void)
{
    short n = *(volatile short *)0x01D2, i;          /* UnitNtryCnt */
    for (i = 0; i < n; i++) {
        uint8_t **h = (uint8_t **)UTableBase[i], *dce, *drv;
        if (!h || !*h) continue;
        dce = strip24(*h);
        if (!(dce[5] & 0x20)) continue;              /* dOpened */
        drv = unit_header((short)~i);
        if (drv[DRVR_NAME] == 6 && memcmp(drv + DRVR_NAME + 1, ".DNET0", 6) == 0) return (short)~i;
    }
    return 0;
}

static int16_t spy_real_open(void *dce)
{
    uint8_t *ours = strip24(*(void **)dce) + DRVR_NAME;
    THz z = GetZone();
    short rf = CurResFile();
    short ref = opened_dnet0();
    int16_t r;
    if (ref) { spy_real_ref = ref; return 0; }
    ours[1] = '!';
    SetZone(LMSysZone);
    UseResFile(LMSysMap);
    r = (int16_t)OpenDriver((ConstStr255Param)"\p.ENET0", &ref);
    UseResFile(rf);
    SetZone(z);
    ours[1] = '.';
    if (r) return r;
    unit_header(ref)[DRVR_NAME + 2] = 'D';      /* ".ENET0" -> ".DNET0" */
    spy_real_ref = ref;
    if (!spy_scsi_orig) {
        spy_scsi_orig = (uint32_t)GetToolTrapAddress(0xA815);
        SetToolTrapAddress((UniversalProcPtr)spy_scsi_patch, 0xA815);
    }
    return 0;
}

/* ---- driver entry points (from spy_drv_stubs.s dispatch) ---- */

int16_t spy_open(void *pb, void *dce);
int16_t spy_open(void *pb, void *dce)
{
    SpyEntry *e;
    spy_our_dce = dce;
    e = log_call(K_OPEN, (EPB *)pb, get_sr());
    if (spy_real_ref) return 0;
    return log_result(e, spy_real_open(dce));
}

int16_t spy_close(void *pb, void *dce);
int16_t spy_close(void *pb, void *dce)
{
    (void)dce;
    log_call(K_CLOSE, (EPB *)pb, get_sr());
    /* Do not close the real driver here: MacTCP may reopen. */
    return 0;
}

int16_t spy_status(void *pb, void *dce);
int16_t spy_status(void *pb, void *dce)
{
    EPB *p = (EPB *)pb;
    SpyEntry *e;
    (void)dce;
    if ((uint16_t)p->csCode == SPY_PRIV_RING) {
        /* SpyDump's private request: hand back the ring pointer, do NOT forward. */
        p->ePointer = (uint32_t)(uintptr_t)&g_ring;
        return 0;
    }
    e = log_call(K_STATUS, p, get_sr());
    return log_result(e, forward_sync(p, 1));
}

int16_t spy_prime(void *pb, void *dce);
int16_t spy_prime(void *pb, void *dce)
{
    SpyEntry *e;
    (void)dce;
    e = log_call(K_PRIME, (EPB *)pb, get_sr());
    return log_result(e, -17);          /* controlErr: Ethernet drivers have no prime */
}

int16_t spy_control(void *pb, void *dce);
int16_t spy_control(void *pb, void *dce)
{
    EPB *p = (EPB *)pb;
    SpyEntry *e;
    uint32_t saved;
    int16_t r;
    spy_our_dce = dce;
    e = log_call(K_CONTROL, p, get_sr());

    if ((uint16_t)p->csCode == E_ATTACH) {
        /* wrap: save MacTCP's handler, attach spy_ph in its place to the real driver */
        hand_add((uint16_t)p->eProtType, p->ePointer);
        saved = p->ePointer;
        if (saved) p->ePointer = (uint32_t)(uintptr_t)spy_ph;
        r = forward_sync(p, 0);
        p->ePointer = saved;
        return log_result(e, r);
    }
    if ((uint16_t)p->csCode == E_DETACH) {
        hand_del((uint16_t)p->eProtType);
        return log_result(e, forward_sync(p, 0));
    }
    if ((uint16_t)p->csCode == E_WRITE) {
        return forward_ewrite_async(p);   /* 1 = complete later */
    }
    return log_result(e, forward_sync(p, 0));   /* EGetInfo, ESetGeneral, EAddMulti/EDelMulti */
}

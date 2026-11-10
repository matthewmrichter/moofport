/* scsiprobe.c - ScsiProbe: raw SCSI-Manager timing probes of the BlueSCSI DaynaPORT.
 *
 * Phase 0 hardware session (coordinator addendum; docs/dev/RESEARCH.md section 2 & 7). It
 * runs with MacTCP CLOSED and does NOT touch .ENET0: it talks to the DaynaPORT target
 * directly through the ORIGINAL (pre-4.3) SCSI Manager, the way the BlueSCSI Wi-Fi DA does.
 * Every probe logs its timing (ticks), result codes and any SCSI Manager error to
 * ScsiProbe.out on the boot volume. These numbers only mean something on the real Classic:
 * Snow does not model SCSI timing, the 75/300 us gaps, or bus errors from blind transfers.
 *
 * Facts that shape it (docs/dev/RESEARCH.md):
 *  (a) even blind TIB instructions POLL the first byte of each scInc/scNoInc/scComp, so a
 *      device pause must fall on a TIB-instruction boundary. Apple assumed gaps <= 16 us
 *      (DV16); the firmware pauses 75 us after the 6-byte header and 300 us between packets.
 *  (b) DV14: several SCSIRead/SCSIRBlind calls after one SCSICmd "may not work" -> tested here.
 *  (c) the DA sequence per command: SCSIGet, SCSISelect, SCSICmd, a TIB, SCSIComplete(...,300).
 *
 * Each SCSI transaction here is exactly that sequence, and always ends with SCSIComplete
 * (RESEARCH.md: never hold the bus). SCSIGet is retried on scMgrBusyErr(7)/scArbNBErr(3).
 *
 * Opt-in: the bus-error-prone / destructive probes (blind reads/writes, the multi-packet
 * variants, the File-Manager-contention probe) run ONLY when selected, because a blind
 * transfer that outruns the device can bus-error or HANG the machine with no software
 * recovery but a restart. Selection is a decimal step bitmask in the first line of a text
 * file "ScsiProbe.cfg" next to the app (bit N = step N). With no cfg, only the safe steps
 * run: 1 (find), 2 (enable/MAC), 3 (empty READ timing), 7a (polled throughput) and 8 (empty
 * READ split into its SCSI Manager calls). 9 (the same READ on the 53C80's registers) is opt-in. The
 * tester enables 4/5/6/7b deliberately and should be ready to restart the Mac. The same line
 * may go on with this Mac's address and the gateway's: "142 192.168.1.50 192.168.1.1". The
 * defaults are Snow's; a real router only answers an address on its own subnet.
 *
 * Probe 7 needs full-size frames, and the only thing that reliably sends them is the gateway
 * answering a 1514-byte ICMP echo. Replies queue in the device, then a burst of READs is timed
 * on its own, so the number is the bus transfer alone.
 *
 * The CDB formats are from docs/dev/PLAN.md section 1.3 / RESEARCH.md.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Memory.h>
#include <Files.h>
#include <OSUtils.h>
#include <Timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runid.h"
#include <stdint.h>

QDGlobals qd;

/* The original SCSI Manager. Multiversal has no SCSI.h, so these are declared from Inside
 * Macintosh: Devices: trap _SCSIDispatch (0xA815), selector word pushed last. */
#pragma pack(push, 2)
typedef struct SCSIInstr { short scOpcode; long scParam1; long scParam2; } SCSIInstr;
#pragma pack(pop)
enum { scInc = 1, scNoInc = 2, scAdd = 3, scMove = 4, scLoop = 5, scNop = 6, scStop = 7, scComp = 8 };

pascal OSErr SCSIReset(void) M68K_INLINE(0x4267, 0xA815);
pascal OSErr SCSIGet(void) M68K_INLINE(0x3F3C, 0x0001, 0xA815);
pascal OSErr SCSISelect(short targetID) M68K_INLINE(0x3F3C, 0x0002, 0xA815);
pascal OSErr SCSICmd(Ptr buffer, short count) M68K_INLINE(0x3F3C, 0x0003, 0xA815);
pascal OSErr SCSIComplete(short *stat, short *message, unsigned long wait)
    M68K_INLINE(0x3F3C, 0x0004, 0xA815);
pascal OSErr SCSIRead(Ptr tibPtr) M68K_INLINE(0x3F3C, 0x0005, 0xA815);
pascal OSErr SCSIWrite(Ptr tibPtr) M68K_INLINE(0x3F3C, 0x0006, 0xA815);
pascal OSErr SCSIRBlind(Ptr tibPtr) M68K_INLINE(0x3F3C, 0x0008, 0xA815);
pascal OSErr SCSIWBlind(Ptr tibPtr) M68K_INLINE(0x3F3C, 0x0009, 0xA815);

/* SCSI Manager error codes (RESEARCH.md / IM-D Devices-150) */
#define scCommErr        2
#define scPhaseErr       5
#define scArbNBErr       3
#define scMgrBusyErr     7
#define scBusTOErr       9
#define scComplPhaseErr 10

#define FRAME_LEN  1514
#define N_EMPTY    64              /* empty READ(6)s to time */
#define N_TRIGGER  6               /* ARP requests to trigger queued replies */

/* step bits */
#define STEP_FIND    (1u << 1)
#define STEP_ENABLE  (1u << 2)
#define STEP_EMPTY   (1u << 3)
#define STEP_WRITE   (1u << 4)     /* opt-in: polled vs blind 1514-byte WRITE */
#define STEP_MULTI   (1u << 5)     /* opt-in: multi-packet READ variants A/B/C */
#define STEP_FMGR    (1u << 6)     /* opt-in: SCSIGet from a TM task vs File Manager */
#define STEP_TPUT    (1u << 7)     /* 7a polled always; 7b blind opt-in via STEP_WRITE */
#define STEP_SPLIT   (1u << 8)     /* empty READ split into SCSI Manager calls (hw4) */
#define STEP_RAW     (1u << 9)     /* opt-in: empty READ on the 53C80 registers (hw4) */

static short g_outref = -1;
static Str255 g_outname;      /* "<Tool>-<run id>.out", see runid.c */
static char g_line[256];
static short g_id = -1;            /* the DaynaPORT's SCSI ID, found by INQUIRY */
static uint32_t g_steps = STEP_FIND | STEP_ENABLE | STEP_EMPTY | STEP_TPUT | STEP_SPLIT;
static uint32_t g_my_ip = 0x0A00004Du;     /* 10.0.0.77 */
static uint32_t g_gw_ip = 0x0A000001u;     /* 10.0.0.1 (Snow NAT gateway) */

static unsigned long ticks(void) { return *(volatile unsigned long *)0x16A; }
static void wr(const char *s) { long n = (long)strlen(s); if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s); }
#define LOG(...) do { sprintf(g_line, __VA_ARGS__); wr(g_line); } while (0)

/* Put the log on disk now: a probe can hang the Mac, and the trail must say which one. */
static void flush_log(void)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = g_outref;
    if (g_outref >= 0) PBFlushFileSync(&pb);
    FlushVol(0, 0);
}

/* ---- SCSI transaction helpers (the DA sequence) ---- */

/* SCSIGet with a couple of retries on busy/arbitration (RESEARCH.md recommendation 1). */
static OSErr scsi_get_retry(void)
{
    OSErr e;
    int i;
    for (i = 0; i < 3; i++) {
        e = SCSIGet();
        if (e != scMgrBusyErr && e != scArbNBErr) return e;
    }
    return e;
}

/* SCSIGet, SCSISelect(id), SCSICmd(cdb,6). Returns the first error, 0 on success. On
 * success the caller performs its TIB read/write then MUST call scsi_finish(). */
static OSErr scsi_begin(short id, const uint8_t *cdb, short cdblen)
{
    OSErr e;
    e = scsi_get_retry();
    if (e) return e;
    e = SCSISelect(id);
    if (e) { short st, msg; SCSIComplete(&st, &msg, 2); return e; }
    e = SCSICmd((Ptr)cdb, cdblen);
    if (e) { short st, msg; SCSIComplete(&st, &msg, 2); return e; }
    return noErr;
}

static OSErr scsi_finish(short *stat, short *message)
{
    return SCSIComplete(stat, message, 300);   /* 300 ticks, as the DA uses */
}

/* One polled read of n bytes into buf: single scInc then scStop. */
static OSErr tib_read_polled(uint8_t *buf, long n)
{
    SCSIInstr tib[2];
    tib[0].scOpcode = scInc; tib[0].scParam1 = (long)(uintptr_t)buf; tib[0].scParam2 = n;
    tib[1].scOpcode = scStop; tib[1].scParam1 = 0; tib[1].scParam2 = 0;
    return SCSIRead((Ptr)tib);
}

/* Header-then-payload split in ONE TIB program, blind: scInc header (first byte polled),
 * then scInc payload (its first byte polled, sitting on the device pause boundary), scStop. */
static OSErr tib_read_blind_split(uint8_t *buf, long hdr, long payload)
{
    SCSIInstr tib[3];
    tib[0].scOpcode = scInc; tib[0].scParam1 = (long)(uintptr_t)buf; tib[0].scParam2 = hdr;
    tib[1].scOpcode = scInc; tib[1].scParam1 = (long)(uintptr_t)(buf + hdr); tib[1].scParam2 = payload;
    tib[2].scOpcode = scStop; tib[2].scParam1 = 0; tib[2].scParam2 = 0;
    return SCSIRBlind((Ptr)tib);
}

static OSErr tib_write_polled(const uint8_t *buf, long n)
{
    SCSIInstr tib[2];
    tib[0].scOpcode = scInc; tib[0].scParam1 = (long)(uintptr_t)buf; tib[0].scParam2 = n;
    tib[1].scOpcode = scStop; tib[1].scParam1 = 0; tib[1].scParam2 = 0;
    return SCSIWrite((Ptr)tib);
}

static OSErr tib_write_blind(const uint8_t *buf, long n)
{
    SCSIInstr tib[2];
    tib[0].scOpcode = scInc; tib[0].scParam1 = (long)(uintptr_t)buf; tib[0].scParam2 = n;
    tib[1].scOpcode = scStop; tib[1].scParam1 = 0; tib[1].scParam2 = 0;
    return SCSIWBlind((Ptr)tib);
}

/* ---- CDB builders ---- */
static void cdb_inquiry(uint8_t *c, uint8_t len) { c[0] = 0x12; c[1] = c[2] = c[3] = 0; c[4] = len; c[5] = 0; }
static void cdb_read6(uint8_t *c, uint16_t alloc, uint8_t flags)
{ c[0] = 0x08; c[1] = 0; c[2] = 0; c[3] = (uint8_t)(alloc >> 8); c[4] = (uint8_t)alloc; c[5] = flags; }
static void cdb_write6(uint8_t *c, uint16_t cnt, uint8_t flags)
{ c[0] = 0x0A; c[1] = 0; c[2] = 0; c[3] = (uint8_t)(cnt >> 8); c[4] = (uint8_t)cnt; c[5] = flags; }
static void cdb_enable(uint8_t *c) { c[0] = 0x0E; c[1] = c[2] = c[3] = c[4] = 0; c[5] = 0x80; }
static void cdb_readmac(uint8_t *c) { c[0] = 0x09; c[1] = c[2] = c[3] = 0; c[4] = 18; c[5] = 0; }

/* Build a broadcast ARP request for the gateway, padded with zeros to FRAME_LEN. */
static void build_arp(uint8_t *f, const uint8_t *mymac)
{
    uint32_t myip = g_my_ip, gw = g_gw_ip;
    int i;
    memset(f, 0, FRAME_LEN);
    for (i = 0; i < 6; i++) f[i] = 0xFF;
    memcpy(f + 6, mymac, 6);
    f[12] = 0x08; f[13] = 0x06;
    f[14] = 0x00; f[15] = 0x01; f[16] = 0x08; f[17] = 0x00; f[18] = 6; f[19] = 4;
    f[20] = 0x00; f[21] = 0x01;
    memcpy(f + 22, mymac, 6);
    f[28] = (uint8_t)(myip >> 24); f[29] = (uint8_t)(myip >> 16);
    f[30] = (uint8_t)(myip >> 8);  f[31] = (uint8_t)myip;
    f[38] = (uint8_t)(gw >> 24); f[39] = (uint8_t)(gw >> 16);
    f[40] = (uint8_t)(gw >> 8);  f[41] = (uint8_t)gw;
}

/* ---- probe 1: find the device ---- */
static int probe_find(uint8_t *mac_out)
{
    short id;
    uint8_t cdb[6], inq[36];
    SCSIReset();
    for (id = 0; id <= 6; id++) {
        OSErr e;
        short st = 0, msg = 0;
        memset(inq, 0, sizeof inq);
        cdb_inquiry(cdb, 36);
        e = scsi_begin(id, cdb, 6);
        if (e) continue;
        e = tib_read_polled(inq, 36);
        scsi_finish(&st, &msg);
        if (e) continue;
        if (memcmp(inq + 8, "Dayna", 5) == 0 && memcmp(inq + 16, "SCSI/Link", 9) == 0) {
            g_id = id;
            LOG("probe 1: found Dayna SCSI/Link at ID %d\r", (int)id);
            (void)mac_out;
            return 1;
        }
    }
    LOG("probe 1: no Dayna SCSI/Link found on IDs 0-6\r");
    return 0;
}

/* ---- probe 2: enable + read MAC ---- */
static void probe_enable(uint8_t *mac_out)
{
    uint8_t cdb[6], info[18];
    OSErr e;
    short st = 0, msg = 0;
    cdb_enable(cdb);
    e = scsi_begin(g_id, cdb, 6);
    if (!e) { /* enable has no data phase; go straight to complete */ scsi_finish(&st, &msg); }
    LOG("probe 2: enable (0x0E) err=%d stat=%d\r", (int)e, (int)st);

    memset(info, 0, sizeof info);
    cdb_readmac(cdb);
    e = scsi_begin(g_id, cdb, 6);
    if (!e) { e = tib_read_polled(info, 18); scsi_finish(&st, &msg); }
    if (!e) {
        memcpy(mac_out, info, 6);
        LOG("probe 2: MAC %02x:%02x:%02x:%02x:%02x:%02x (err=%d)\r",
            info[0], info[1], info[2], info[3], info[4], info[5], (int)e);
    } else {
        LOG("probe 2: read MAC (0x09) err=%d\r", (int)e);
    }
}

/* ---- probe 3: time N empty polled READ(6)s ---- */
static void probe_empty_read(void)
{
    uint8_t cdb[6], buf[64];
    int i, empties = 0;
    unsigned long t0 = ticks();
    for (i = 0; i < N_EMPTY; i++) {
        OSErr e;
        short st = 0, msg = 0;
        cdb_read6(cdb, 6, 0x80);          /* ask one packet; empty queue returns 6 zero bytes */
        e = scsi_begin(g_id, cdb, 6);
        if (!e) { e = tib_read_polled(buf, 6); scsi_finish(&st, &msg); }
        if (!e && buf[0] == 0 && buf[1] == 0) empties++;
    }
    {
        unsigned long dt = ticks() - t0;
        LOG("probe 3: %d empty READ(6)s polled in %lu ticks (%lu.%02lu ticks each), %d empty\r",
            N_EMPTY, dt, dt / N_EMPTY, (dt * 100 / N_EMPTY) % 100, empties);
    }
}

/* ---- probe 4: send a 1514-byte frame, polled TIB vs blind TIB ---- */
static void probe_write(const uint8_t *mymac)
{
    static uint8_t frame[FRAME_LEN];
    static uint8_t out[4 + FRAME_LEN + 4];
    uint8_t cdb[6];
    OSErr e;
    short st, msg;
    unsigned long t0;

    build_arp(frame, mymac);
    /* universal single-frame format: [len_hi len_lo 0 0][frame][0 0 0 0], CDB[5]=0x80 */
    out[0] = (uint8_t)(FRAME_LEN >> 8); out[1] = (uint8_t)FRAME_LEN; out[2] = 0; out[3] = 0;
    memcpy(out + 4, frame, FRAME_LEN);
    memset(out + 4 + FRAME_LEN, 0, 4);

    cdb_write6(cdb, FRAME_LEN, 0x80);
    t0 = ticks();
    e = scsi_begin(g_id, cdb, 6);
    if (!e) { e = tib_write_polled(out, sizeof out); scsi_finish(&st, &msg); }
    LOG("probe 4: polled WRITE 1514-byte ARP: err=%d in %lu ticks\r", (int)e, (unsigned long)(ticks() - t0));

    cdb_write6(cdb, FRAME_LEN, 0x80);
    t0 = ticks();
    e = scsi_begin(g_id, cdb, 6);
    if (!e) { e = tib_write_blind(out, sizeof out); scsi_finish(&st, &msg); }
    LOG("probe 4: BLIND WRITE 1514-byte ARP: err=%d in %lu ticks (watch for bus error / freeze)\r",
        (int)e, (unsigned long)(ticks() - t0));
}

/* ---- probe 5: multi-packet READ (CDB[5]=0xC0) after triggering replies ---- */
static void trigger_arps(const uint8_t *mymac)
{
    static uint8_t frame[FRAME_LEN];
    static uint8_t out[4 + FRAME_LEN + 4];
    uint8_t cdb[6];
    int i;
    build_arp(frame, mymac);
    out[0] = (uint8_t)(FRAME_LEN >> 8); out[1] = (uint8_t)FRAME_LEN; out[2] = 0; out[3] = 0;
    memcpy(out + 4, frame, FRAME_LEN);
    memset(out + 4 + FRAME_LEN, 0, 4);
    for (i = 0; i < N_TRIGGER; i++) {
        OSErr e;
        short st, msg;
        cdb_write6(cdb, FRAME_LEN, 0x80);
        e = scsi_begin(g_id, cdb, 6);
        if (!e) { tib_write_polled(out, sizeof out); scsi_finish(&st, &msg); }
    }
    /* let the replies arrive and queue in the device */
    { unsigned long t0 = ticks(); while (ticks() - t0 < 60) { } }
}

static void probe_multi(const uint8_t *mymac)
{
    static uint8_t buf[4096];
    uint8_t cdb[6];
    OSErr e;
    short st, msg;

    /* Variant C: everything polled (the safe reference). */
    trigger_arps(mymac);
    cdb_read6(cdb, 3000, 0xC0);           /* several packets per READ */
    e = scsi_begin(g_id, cdb, 6);
    if (!e) { e = tib_read_polled(buf, 3000); scsi_finish(&st, &msg); }
    LOG("probe 5C: multi READ all-polled: err=%d first6=%02x%02x%02x%02x%02x%02x\r",
        (int)e, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);

    /* Variant A: one TIB program, 6-byte header polled then a long blind read. */
    trigger_arps(mymac);
    cdb_read6(cdb, 3000, 0xC0);
    e = scsi_begin(g_id, cdb, 6);
    if (!e) { e = tib_read_blind_split(buf, 6, 2994); scsi_finish(&st, &msg); }
    LOG("probe 5A: multi READ header-polled+blind-rest: err=%d first6=%02x%02x%02x%02x%02x%02x\r",
        (int)e, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);

    /* Variant B: TWO SCSIRead calls after ONE SCSICmd (the DV14 "may not work" case). */
    trigger_arps(mymac);
    cdb_read6(cdb, 3000, 0xC0);
    e = scsi_begin(g_id, cdb, 6);
    if (!e) {
        OSErr e1 = tib_read_polled(buf, 6);           /* first call: header */
        OSErr e2 = tib_read_polled(buf + 6, 2994);    /* second call: payload (DV14 hazard) */
        scsi_finish(&st, &msg);
        LOG("probe 5B: two SCSIRead calls per command (DV14): e1=%d e2=%d first6=%02x%02x%02x%02x%02x%02x\r",
            (int)e1, (int)e2, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    } else {
        LOG("probe 5B: begin err=%d\r", (int)e);
    }
}

/* ---- probe 6: SCSIGet from a Time Manager task while the app does a big File Manager read ---- */
extern void sp_tm_entry(void);
static TMTask g_tm;
static volatile long g_tm_got, g_tm_busy, g_tm_other, g_tm_runs;
static volatile int g_tm_stop, g_tm_stuck;

/* One whole empty READ from the task, as the driver's poll task would do it. Not SCSIGet alone
 * then SCSIComplete: the task runs at mask 1, Ticks cannot advance there, so SCSIComplete only
 * returns if the device really finishes. If selection fails the transaction is left for the
 * main line to end. */
void sp_tm_tick(void);   /* called from sp_tm_entry (asm) */
void sp_tm_tick(void)
{
    static uint8_t cdb[6], hdr[6];
    OSErr e;
    g_tm_runs++;
    e = SCSIGet();
    if (e == noErr) {
        short st, msg;
        cdb_read6(cdb, 6, 0x80);
        if (SCSISelect(g_id) == noErr && SCSICmd((Ptr)cdb, 6) == noErr) {
            tib_read_polled(hdr, 6);
            SCSIComplete(&st, &msg, 1);
            g_tm_got++;
        } else {
            g_tm_stuck = 1;
            g_tm_stop = 1;
        }
    } else if (e == scMgrBusyErr) {
        g_tm_busy++;
    } else {
        g_tm_other++;
    }
    if (!g_tm_stop) PrimeTime((QElemPtr)&g_tm, 2);   /* re-arm ~2 ms */
}

static void probe_fmgr(void)
{
    OSErr e;
    short fref = -1;
    static uint8_t fbuf[16384];
    long total = 0;

    g_tm_got = g_tm_busy = g_tm_other = g_tm_runs = 0;
    g_tm_stop = g_tm_stuck = 0;
    memset(&g_tm, 0, sizeof g_tm);
    g_tm.tmAddr = (ProcPtr)sp_tm_entry;
    InsTime((QElemPtr)&g_tm);
    PrimeTime((QElemPtr)&g_tm, 2);

    /* Keep the disk busy for ~3 s by reading this app's own resource fork in chunks too big for
     * the volume cache. The name comes from CurApName; the app's folder is the default volume. */
    {
        unsigned long t0 = ticks();
        ParamBlockRec pb;
        memset(&pb, 0, sizeof pb);
        pb.ioParam.ioNamePtr = (StringPtr)0x910;
        pb.ioParam.ioPermssn = fsRdPerm;        /* the Resource Manager already has it open */
        e = PBOpenRFSync(&pb);
        if (e == noErr) {
            fref = pb.ioParam.ioRefNum;
            while (ticks() - t0 < 180) {
                long len = (long)sizeof fbuf;
                e = FSRead(fref, &len, (Ptr)fbuf);
                total += len;
                if (e) SetFPos(fref, fsFromStart, 0);
            }
            FSClose(fref);
        } else {
            LOG("probe 6: could not open this app's resource fork: %d\r", (int)e);
        }
    }

    g_tm_stop = 1;
    { unsigned long t0 = ticks(); while (ticks() - t0 < 12) { } }   /* let the last task fire */
    RmvTime((QElemPtr)&g_tm);
    if (g_tm_stuck) { short st, msg; SCSIComplete(&st, &msg, 2); LOG("probe 6: selection failed in the task\r"); }

    LOG("probe 6: TM-task SCSIGet during File Manager read: runs=%ld got=%ld busy(scMgrBusyErr)=%ld other=%ld; app read %ld bytes\r",
        g_tm_runs, g_tm_got, g_tm_busy, g_tm_other, total);
}

/* ---- probe 7: polled vs blind throughput ---- */
#define BURST     16               /* frames per burst (the device queue holds at most 19) */
#define ROUNDS     4

static uint8_t g_gwmac[6];

static uint16_t cksum(const uint8_t *p, int n)
{
    uint32_t acc = 0;
    int i;
    for (i = 0; i + 1 < n; i += 2) acc += ((uint32_t)p[i] << 8) | p[i + 1];
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)~acc;
}

/* One frame through the universal single-frame WRITE. `out` has 4 bytes of room in front of
 * the frame at out + 4, and 4 behind it. */
static OSErr send_frame(uint8_t *out, uint16_t len)
{
    uint8_t cdb[6];
    OSErr e;
    short st, msg;
    out[0] = (uint8_t)(len >> 8); out[1] = (uint8_t)len; out[2] = 0; out[3] = 0;
    memset(out + 4 + len, 0, 4);
    cdb_write6(cdb, len, 0x80);
    e = scsi_begin(g_id, cdb, 6);
    if (!e) { e = tib_write_polled(out, 4L + len + 4); scsi_finish(&st, &msg); }
    return e;
}

/* ARP for the gateway and keep its MAC; returns 1 when it answered. */
static int learn_gateway(const uint8_t *mymac)
{
    static uint8_t out[4 + FRAME_LEN + 4];
    static uint8_t buf[2048];
    uint8_t cdb[6];
    unsigned long t0 = ticks();
    build_arp(out + 4, mymac);
    if (send_frame(out, 60)) return 0;
    while (ticks() - t0 < 120) {
        OSErr e;
        short st, msg;
        cdb_read6(cdb, 1600, 0x80);
        e = scsi_begin(g_id, cdb, 6);
        if (e) continue;
        e = tib_read_polled(buf, 1600);
        scsi_finish(&st, &msg);
        if (e && e != scPhaseErr) continue;
        if (buf[6 + 12] == 0x08 && buf[6 + 13] == 0x06 && buf[6 + 21] == 2
            && memcmp(buf + 6 + 28, out + 4 + 38, 4) == 0) {
            memcpy(g_gwmac, buf + 6 + 22, 6);
            return 1;
        }
    }
    return 0;
}

/* A 1514-byte ICMP echo request to the gateway, built at out + 4. */
static void build_echo(uint8_t *out, const uint8_t *mymac, uint16_t seq)
{
    uint8_t *f = out + 4, *ip = f + 14, *icmp = ip + 20;
    uint16_t c;
    memset(f, 0, FRAME_LEN);
    memcpy(f, g_gwmac, 6);
    memcpy(f + 6, mymac, 6);
    f[12] = 0x08; f[13] = 0x00;
    ip[0] = 0x45; ip[2] = 1500 >> 8; ip[3] = 1500 & 0xFF;
    ip[4] = (uint8_t)(seq >> 8); ip[5] = (uint8_t)seq;
    ip[8] = 64; ip[9] = 1;
    ip[12] = (uint8_t)(g_my_ip >> 24); ip[13] = (uint8_t)(g_my_ip >> 16);
    ip[14] = (uint8_t)(g_my_ip >> 8);  ip[15] = (uint8_t)g_my_ip;
    ip[16] = (uint8_t)(g_gw_ip >> 24); ip[17] = (uint8_t)(g_gw_ip >> 16);
    ip[18] = (uint8_t)(g_gw_ip >> 8);  ip[19] = (uint8_t)g_gw_ip;
    c = cksum(ip, 20); ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    icmp[0] = 8;
    icmp[4] = (uint8_t)(seq >> 8); icmp[5] = (uint8_t)seq;
    icmp[6] = (uint8_t)(seq >> 8); icmp[7] = (uint8_t)seq;
    c = cksum(icmp, 1480); icmp[2] = (uint8_t)(c >> 8); icmp[3] = (uint8_t)c;
}

/* Queue BURST replies in the device, then time one READ per reply. Polled: one call asking for
 * exactly a 1514-byte reply (header 6, frame 1514, CRC 4): asking for more leaves the SCSI
 * Manager waiting for bytes that never come, and that wait would be the measurement. Blind: the
 * header is read polled, so the blind read asks for exactly what the device will send (a blind
 * read past the end of the data can hang the Mac). That is two SCSI Manager calls per command,
 * the DV14 case, so run it only after probe 5B shows that works. Only reads that returned a
 * frame are timed: asking for a frame the device does not have ends on a SCSI Manager timeout
 * of about 3 s, and one lost reply would otherwise swamp the figure. */
static void burst(const uint8_t *mymac, int blind, const char *name)
{
    static uint8_t out[4 + FRAME_LEN + 4];
    static uint8_t buf[2048];
    uint8_t cdb[6];
    int r, i;
    long bytes = 0, frames = 0, seq = 0;
    unsigned long dt = 0;
    OSErr e = noErr;

    for (r = 0; r < ROUNDS && !e; r++) {
        unsigned long t0;
        for (i = 0; i < BURST; i++) {
            build_echo(out, mymac, (uint16_t)(seq++));
            send_frame(out, FRAME_LEN);
        }
        t0 = ticks();
        while (ticks() - t0 < 60) { }       /* let the replies queue in the device */
        for (i = 0; i < BURST; i++) {
            short st, msg;
            long got;
            t0 = ticks();
            cdb_read6(cdb, 1600, 0x80);
            e = scsi_begin(g_id, cdb, 6);
            if (e) break;
            if (blind) {
                SCSIInstr tib[2];
                e = tib_read_polled(buf, 6);
                got = ((long)buf[0] << 8) | buf[1];
                if (!e && got > 0 && got <= 1600) {
                    tib[0].scOpcode = scInc; tib[0].scParam1 = (long)(uintptr_t)(buf + 6); tib[0].scParam2 = got;
                    tib[1].scOpcode = scStop; tib[1].scParam1 = 0; tib[1].scParam2 = 0;
                    e = SCSIRBlind((Ptr)tib);
                }
            } else {
                e = tib_read_polled(buf, 6L + FRAME_LEN + 4);
                if (e == scPhaseErr) e = noErr;
                got = ((long)buf[0] << 8) | buf[1];
            }
            scsi_finish(&st, &msg);
            if (e || got == 0) break;
            dt += ticks() - t0;
            bytes += got;
            frames++;
        }
    }
    LOG("probe %s: %ld bytes in %ld frames, %lu ticks (%lu B/s), err=%d\r", name, bytes, frames, dt,
        dt ? (unsigned long)(bytes / (long)dt * 60) : 0, (int)e);
}

static void probe_throughput(const uint8_t *mymac, int do_blind)
{
    if (!learn_gateway(mymac)) {
        LOG("probe 7: gateway %08lx did not answer ARP; check the addresses in ScsiProbe.cfg\r",
            (unsigned long)g_gw_ip);
        return;
    }
    burst(mymac, 0, "7a polled");
    if (do_blind) burst(mymac, 1, "7b header-polled, payload-blind");
}

/* ---- probes 8 and 9: where the 3.5 ms of an empty READ go (hw4) ----
 *
 * hw3: polling more often took CPU from MacTCP because one empty READ costs about 3.5 ms on the
 * Classic (probe 3, hw1). Probe 8 splits one empty READ through the SCSI Manager into SCSIGet,
 * SCSISelect, SCSICmd, the 6-byte SCSIRead and SCSIComplete. Probe 9 (opt-in, bit 9) does the
 * same empty READ straight on the 53C80's registers, split into arbitration, selection, command
 * and data/status/message, with interrupts masked for the transaction.
 *
 * Timing: the revised Time Manager (System 6.0.3+). A task primed with a negative count (us)
 * and removed with RmvTime before it fires gets the time left in tmCount, so elapsed = primed
 * - left, to tens of microseconds. Each step is a lap (stop, then start again); the cost of a
 * lap itself is measured first (calibration) and subtracted. Every figure is reported as min /
 * median / max over N_LAP runs: an interrupt (the Mac's own tick, MoofPort's poll) inflates a
 * single run, the minimum is the clean cost. Snow does not model SCSI timing, so only the
 * hardware numbers mean anything; in Snow these probes only show that the code runs.
 *
 * Run MoofTune "quiet" first if MoofPort is installed, so its poll stays off the bus.
 *
 * 53C80 addresses on the Mac Plus/SE/Classic: base 0x580000 (low memory SCSIBase), register n
 * at base + 16n, read on the even address and written on the odd one (/IOR is /UDS, /IOW is
 * /LDS; Snow core/src/mac/compact/bus.rs scsi_cycle, controller.rs). */
extern void sp_clk_nop(void);         /* scsiprobe_tm.s: RTS, in case a lap timer ever fires */
static TMTask g_clk, g_clkw;         /* laps; the "whole READ" lap around raw_transaction's */
#define CLK_US 1000000L

static void clk_start_on(TMTask *c)
{
    memset(c, 0, sizeof *c);
    c->tmAddr = (ProcPtr)sp_clk_nop;
    InsTime((QElemPtr)c);
    PrimeTime((QElemPtr)c, -CLK_US);
}

static long clk_stop_on(TMTask *c)
{
    RmvTime((QElemPtr)c);
    return CLK_US + c->tmCount;       /* tmCount: microseconds left, negative */
}

static void clk_start(void) { clk_start_on(&g_clk); }
static long clk_stop(void) { return clk_stop_on(&g_clk); }

#define N_LAP   32
#define N_PART  6

typedef struct Laps {
    const char *name[N_PART];
    long v[N_PART][N_LAP];
    int n;                             /* runs recorded */
} Laps;

static long g_cal;                     /* median cost of one lap (stop + start) */

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}

static long median(long *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_long);
    return v[n / 2];
}

static void report(const char *title, Laps *L, int parts)
{
    int p;
    LOG("%s: %d runs, lap cost %ld us subtracted (min / median / max us)\r", title, L->n, g_cal);
    for (p = 0; p < parts && L->n > 0; p++) {
        long *v = L->v[p];
        int i;
        for (i = 0; i < L->n; i++) v[i] -= g_cal;
        qsort(v, (size_t)L->n, sizeof *v, cmp_long);
        LOG("  %-28s %6ld %6ld %6ld\r", L->name[p], v[0], v[L->n / 2], v[L->n - 1]);
    }
}

static void calibrate(void)
{
    long v[N_LAP];
    int i;
    for (i = 0; i < N_LAP; i++) { clk_start(); v[i] = clk_stop(); }
    g_cal = median(v, N_LAP);
    LOG("lap calibration: min %ld, median %ld, max %ld us (Time Manager %s)\r", v[0], g_cal,
        v[N_LAP - 1], (g_cal > 0 && g_cal < 2000) ? "ok" : "SUSPECT: not the revised Time Manager?");
}

static Laps g_laps;

static void probe_empty_split(void)
{
    uint8_t cdb[6], buf[8];
    int i, empties = 0, errs = 0;
    Laps *L = &g_laps;
    memset(L, 0, sizeof *L);
    L->name[0] = "SCSIGet";
    L->name[1] = "SCSISelect";
    L->name[2] = "SCSICmd (READ(6))";
    L->name[3] = "SCSIRead 6-byte header";
    L->name[4] = "SCSIComplete";
    L->name[5] = "whole READ (one lap)";
    calibrate();
    cdb_read6(cdb, 6, 0x80);              /* as probe 3: one packet, header only when empty */
    for (i = 0; i < N_LAP; i++) {
        OSErr e;
        short st = 0, msg = 0;
        long t[5];
        memset(buf, 0xEE, sizeof buf);
        clk_start(); e = SCSIGet(); t[0] = clk_stop();
        if (e) { errs++; continue; }
        clk_start(); e = SCSISelect(g_id); t[1] = clk_stop();
        if (e) { SCSIComplete(&st, &msg, 2); errs++; continue; }
        clk_start(); e = SCSICmd((Ptr)cdb, 6); t[2] = clk_stop();
        if (e) { SCSIComplete(&st, &msg, 2); errs++; continue; }
        clk_start(); e = tib_read_polled(buf, 6); t[3] = clk_stop();
        clk_start(); SCSIComplete(&st, &msg, 300); t[4] = clk_stop();
        if (e) { errs++; continue; }
        if (buf[0] == 0 && buf[1] == 0) empties++;
        {
            int p;
            for (p = 0; p < 5; p++) L->v[p][L->n] = t[p];
        }
        /* the same READ again in one lap, for the total without the lap overheads */
        clk_start();
        e = SCSIGet();
        if (!e) {
            if (SCSISelect(g_id) == noErr && SCSICmd((Ptr)cdb, 6) == noErr) tib_read_polled(buf, 6);
            SCSIComplete(&st, &msg, 300);
        }
        L->v[5][L->n] = clk_stop();
        L->n++;
    }
    LOG("probe 8: %d of %d recorded runs were empty, %d runs failed\r", empties, L->n, errs);
    report("probe 8 SCSI Manager empty READ", L, 6);
}

/* ---- the raw 53C80 path (probe 9) ---- */

enum { R_DATA = 0, R_ICR = 1, R_MR = 2, R_TCR = 3, R_CSR = 4, R_BSR = 5, R_RPI = 7 };
enum { ICR_DBUS = 0x01, ICR_SEL = 0x04, ICR_BSY = 0x08, ICR_ACK = 0x10, ICR_LA = 0x20,
       ICR_AIP = 0x40 };
enum { MR_ARB = 0x01 };
enum { CSR_SEL = 0x02, CSR_IO = 0x04, CSR_CD = 0x08, CSR_MSG = 0x10, CSR_REQ = 0x20,
       CSR_BSY = 0x40 };
enum { PH_DATA_IN = 1, PH_CMD = 2, PH_STATUS = 3, PH_MSG_IN = 7 };
#define RAW_SPIN 50000L                 /* register polls before a wait gives up (tens of ms) */

static volatile uint8_t *g_ncr;

static uint8_t nrd(int r) { return g_ncr[r << 4]; }
static void nwr(int r, uint8_t v) { g_ncr[(r << 4) + 1] = v; }

static void raw_release(void)
{
    nwr(R_TCR, 0);
    nwr(R_ICR, 0);
    nwr(R_MR, 0);
}

/* Wait until (CSR & mask) == want; 0 or -1 on timeout. */
static int csr_wait(uint8_t mask, uint8_t want)
{
    long i;
    for (i = 0; i < RAW_SPIN; i++)
        if ((nrd(R_CSR) & mask) == want)
            return 0;
    return -1;
}

/* One handshaked byte in the current phase (TCR already set). */
static int raw_in(uint8_t *b)
{
    *b = nrd(R_DATA);
    nwr(R_ICR, ICR_ACK);
    if (csr_wait(CSR_REQ, 0)) return -1;
    nwr(R_ICR, 0);
    return 0;
}

static int raw_out(uint8_t b)
{
    nwr(R_DATA, b);
    nwr(R_ICR, ICR_DBUS);
    nwr(R_ICR, ICR_DBUS | ICR_ACK);
    if (csr_wait(CSR_REQ, 0)) return -1;
    nwr(R_ICR, 0);
    return 0;
}

/* One transaction: arbitration, selection of `id`, the 6-byte CDB, then whatever phases the
 * target asks for (data in, status, message in) until it lets go of BSY. Laps go to t[0..3].
 * Returns 0, or the stage that failed: -1 bus not free, -2 no arbitration, -3 lost
 * arbitration, -4 no selection, -5 command, -6 unexpected phase, -7 a byte timed out. */
static int raw_transaction(int id, const uint8_t *cdb, uint8_t *in, int inmax, int *got,
                           uint8_t *st, uint8_t *msg, long *t)
{
    int ci = 0, n = 0, i, lap = 2;
    long k;
    *got = 0;
    if (csr_wait(CSR_BSY | CSR_SEL, 0)) return -1;
    clk_start();
    /* arbitration (we are ID 7) */
    nwr(R_DATA, 0x80);
    nwr(R_MR, MR_ARB);
    for (k = 0; k < RAW_SPIN && !(nrd(R_ICR) & ICR_AIP); k++) { }
    if (k == RAW_SPIN) { raw_release(); t[0] = clk_stop(); return -2; }
    for (i = 0; i < 4; i++) (void)nrd(R_CSR);    /* arbitration delay, >= 2.2 us */
    if (nrd(R_ICR) & ICR_LA) { raw_release(); t[0] = clk_stop(); return -3; }
    nwr(R_ICR, ICR_SEL | ICR_BSY);
    t[0] = clk_stop();
    clk_start();
    /* selection */
    nwr(R_DATA, (uint8_t)(0x80 | (1 << id)));
    nwr(R_ICR, ICR_DBUS | ICR_SEL | ICR_BSY);
    nwr(R_MR, 0);
    nwr(R_ICR, ICR_DBUS | ICR_SEL);
    if (csr_wait(CSR_BSY, CSR_BSY)) { raw_release(); t[1] = clk_stop(); return -4; }
    nwr(R_ICR, 0);
    t[1] = clk_stop();
    clk_start();
    /* information transfer phases until the target releases BSY */
    for (;;) {
        uint8_t csr = 0, ph, b;
        for (k = 0; k < RAW_SPIN; k++) {
            csr = nrd(R_CSR);
            if ((csr & CSR_REQ) || !(csr & CSR_BSY)) break;
        }
        if (k == RAW_SPIN) { raw_release(); t[lap] = clk_stop(); return -7; }
        if (!(csr & CSR_BSY)) break;
        ph = (uint8_t)((csr >> 2) & 7);
        nwr(R_TCR, ph);
        if (lap == 2 && ph != PH_CMD) { t[2] = clk_stop(); clk_start(); lap = 3; }
        switch (ph) {
        case PH_CMD:
            if (ci >= 6 || raw_out(cdb[ci++])) { raw_release(); t[lap] = clk_stop(); return -5; }
            break;
        case PH_DATA_IN:
            if (raw_in(&b)) { raw_release(); t[lap] = clk_stop(); return -7; }
            if (n < inmax) in[n++] = b;
            break;
        case PH_STATUS:
            if (raw_in(st)) { raw_release(); t[lap] = clk_stop(); return -7; }
            break;
        case PH_MSG_IN:
            if (raw_in(msg)) { raw_release(); t[lap] = clk_stop(); return -7; }
            break;
        default:
            raw_release();
            t[lap] = clk_stop();
            return -6;
        }
    }
    raw_release();
    t[lap] = clk_stop();
    if (lap == 2) t[3] = 0;
    *got = n;
    return 0;
}

static uint16_t sr_mask(void)
{
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    return sr;
}

static void sr_restore(uint16_t sr) { __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc"); }

static void probe_raw(void)
{
    uint8_t cdb[6], buf[64], st, msg;
    int i, got, rc, empties = 0, errs = 0, last_err = 0;
    long base = *(volatile long *)0x0C00;      /* SCSIBase */
    Laps *L = &g_laps;
    if (base < 0x580000L || base > 0x5FFFFFL) {
        LOG("probe 9: SCSIBase %08lx is not the compact Macs' 53C80; using 00580000\r", base);
        base = 0x580000L;
    }
    g_ncr = (volatile uint8_t *)base;
    LOG("probe 9: 53C80 read base %08lx, write base %08lx (register n at +16n)\r", base, base + 1);
    flush_log();
    memset(L, 0, sizeof *L);
    L->name[0] = "arbitration";
    L->name[1] = "selection";
    L->name[2] = "command (6 bytes)";
    L->name[3] = "data in + status + message";
    L->name[4] = "whole READ (one lap)";
    calibrate();
    cdb_read6(cdb, 6, 0x80);
    for (i = 0; i < N_LAP; i++) {
        long t[4] = {0, 0, 0, 0}, whole;
        uint16_t sr;
        memset(buf, 0xEE, sizeof buf);
        st = msg = 0xFF;
        sr = sr_mask();                 /* nothing may touch the bus in between, MoofPort included */
        rc = raw_transaction(g_id, cdb, buf, (int)sizeof buf, &got, &st, &msg, t);
        sr_restore(sr);
        if (rc) { errs++; last_err = rc; continue; }
        if (got >= 2 && buf[0] == 0 && buf[1] == 0) empties++;
        {
            int p;
            for (p = 0; p < 4; p++) L->v[p][L->n] = t[p];
        }
        sr = sr_mask();
        clk_start_on(&g_clkw);
        rc = raw_transaction(g_id, cdb, buf, (int)sizeof buf, &got, &st, &msg, t);
        whole = clk_stop_on(&g_clkw);
        sr_restore(sr);
        L->v[4][L->n] = whole - 4 * g_cal;   /* the inner laps' cost is not part of the READ */
        L->n++;
        if (i == 0)
            LOG("probe 9: first READ: %d bytes in (%02x %02x %02x %02x %02x %02x), status %02x, "
                "message %02x\r", got, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], st, msg);
    }
    LOG("probe 9: %d of %d recorded runs were empty, %d runs failed%s", empties, L->n, errs,
        errs ? "" : "\r");
    if (errs) LOG(" (last failure: stage %d)\r", last_err);
    report("probe 9 raw 53C80 empty READ", L, 5);
}

static void read_cfg(void)
{
    short r = -1;
    long len = 0;
    char buf[64];
    long v;
    int a, b, c, d, e, f, g, h;
    if (FSOpen((ConstStr255Param)"\pScsiProbe.cfg", 0, &r) != noErr) return;
    GetEOF(r, &len);
    if (len > 63) len = 63;
    FSRead(r, &len, (Ptr)buf);
    buf[len] = 0;
    if (sscanf(buf, "%ld", &v) == 1 && v > 0) g_steps = (uint32_t)v;
    if (sscanf(buf, "%ld %d.%d.%d.%d %d.%d.%d.%d", &v, &a, &b, &c, &d, &e, &f, &g, &h) == 9) {
        g_my_ip = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
        g_gw_ip = ((uint32_t)e << 24) | ((uint32_t)f << 16) | ((uint32_t)g << 8) | (uint32_t)h;
    }
    FSClose(r);
}

int main(void)
{
    Str255 ps;
    size_t n;
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
    runid_name("ScsiProbe", ".out", g_outname);

    FSDelete((ConstStr255Param)g_outname, 0);
    Create((ConstStr255Param)g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStr255Param)g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
    wr("ScsiProbe (raw SCSI Manager probes; MacTCP must be closed; does NOT use .ENET0)\r");

    read_cfg();
    LOG("steps enabled = 0x%03lx (bits 1..9; cfg ScsiProbe.cfg overrides), us %08lx, gateway %08lx\r",
        (unsigned long)g_steps, (unsigned long)g_my_ip, (unsigned long)g_gw_ip);

    if (g_steps & STEP_FIND) {
        if (!probe_find(mac)) { wr("RESULT DONE (no device)\r"); goto fin; }
    } else {
        LOG("probe 1 (find) not selected; cannot run without a device ID\r");
        wr("RESULT DONE\r");
        goto fin;
    }

#define STEP(bit, name, call) \
    if (g_steps & (bit)) { LOG("-- %s\r", name); flush_log(); call; flush_log(); }
    STEP(STEP_ENABLE, "enable and MAC", probe_enable(mac));
    STEP(STEP_EMPTY, "empty READ", probe_empty_read());
    STEP(STEP_WRITE, "1514-byte WRITE", probe_write(mac));
    STEP(STEP_MULTI, "multi-packet READ", probe_multi(mac));
    STEP(STEP_FMGR, "task vs File Manager", probe_fmgr());
    STEP(STEP_TPUT, "throughput", probe_throughput(mac, (g_steps & STEP_WRITE) != 0));
    STEP(STEP_SPLIT, "empty READ split (SCSI Manager)", probe_empty_split());
    STEP(STEP_RAW, "empty READ on the 53C80 registers", probe_raw());

    wr("RESULT DONE\r");
fin:
    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);
    sprintf(g_line, "ScsiProbe finished. Results are in %.*s", (int)g_outname[0], (const char *)g_outname + 1);
    n = strlen(g_line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, g_line, n);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

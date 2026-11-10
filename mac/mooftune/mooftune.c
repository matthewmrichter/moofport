/* mooftune.c - MoofTune: set MoofPort's poll timing, blind payload reads and checksum
 * verification (private Control MP_CTL_TUNE, mp_drv.h) until the next restart, and write the
 * before/after settings and counters to MoofTune-<run id>.out.
 *
 * What to set comes from "MoofTune.cfg" next to the app if it exists (one key=value per line:
 * burst, hold, mid, quiet, idle, step, busy (ms), blind, blind_tx, verify, batch, chase, yield,
 * inline (0/1), early (ms after an inline WRITE: 1, 2, 0 = the usual poll), rx (frames per tick, 2 or 4), ack (ms after a WRITE, 2 or 5), hush (0/1: 15 ms); unnamed keys
 * keep their current value), else from the run id, which then names a preset:
 *     default   MoofPort's defaults (decision 6): 5 ms for 60 ms after traffic, one tick
 *               until 2 s of quiet, then out to three ticks; blind reads, blind frame writes
 *               (send-fix; 1.0b1 wrote polled), batched WRITEs, 2 frames per tick, poll 5 ms after a
 *               WRITE and chase the reply, yield after a READ of replies only; inline writes with
 *               the first READ 2 ms after (since hwsend9), no hush
 *     tick      0.3d3's timing for comparison: one poll per tick, always
 *     phase1    Phase 1/2 behaviour: one poll per tick, polled reads
 *     burst     5 ms for 60 ms after traffic, else one tick (no idle back-off)
 *     idle      one tick, backing off to three ticks after 2 s of quiet (no burst)
 *     txblind   frame bytes of each WRITE with SCSIWBlind (the default since send-fix)
 *     txpolled  frame bytes of each WRITE handshaked byte by byte (1.0b1's writes)
 *     nobatch   the defaults, but one frame per WRITE
 *     batch     the defaults (all queued frames in one WRITE, new firmware)
 *     rx2       the defaults (at most 2 received frames per poll tick, since hwsend7)
 *     rx4       the defaults, but up to 4 received frames per poll tick (hwsend6's default)
 *     inline    the defaults (since hwsend9: an EWrite with nothing queued is written at once, in
 *               the call, and the first READ follows 2 ms later)
 *     noinline  the defaults, but every EWrite is queued for the poll task (hwsend8's control)
 *     chase2    hwsend10's default (a poll right after an inline WRITE, then every 2 ms
 *               for about 8 ms until the reply comes; early2 is the same)
 *     chase1    the same every 1 ms (early1 is the same)
 *     early0    the defaults (since hwsend11: the first poll 5 ms after an inline WRITE)
 *     inhushc1  hush with chase1; inhush0 hush with early0
 *     nopause   the defaults, but no 12 ms pause after the reply to our own WRITE
 *     steady    DaynaPORT's rhythm: a poll every 16 ms, one frame per READ, no yield cut, chase,
 *               pause or early poll after an inline WRITE (hwsend12); steadytx the same with
 *               handshaked frame writes; newtx the defaults with handshaked frame writes
 *     repoll    the defaults, but re-poll 1 ms after a READ the yield cut short
 *     old10     hwsend10's defaults (chase2, no pause, re-poll)
 *     oldhush   hwsend10's inline + hush (inhush0 without the hwsend11 changes)
 *     inack2    the defaults with ack2
 *     hush      the defaults without inline, and no poll for 15 ms after handing MacTCP only small TCP frames
 *               (its ACKs), unless it sends first (hush10/hush20 gone since hwsend11)
 *     inhush    the defaults (inline) with hush and chase2
 *     ack5      the defaults (the first poll 5 ms after a WRITE, no READ sooner)
 *     ack2      the defaults, but the first poll 2 ms after a WRITE (hwsend5's default)
 *     noyield   the defaults, but keep reading after a READ that brought only replies
 *     nochase   the defaults, but no reply chase (one early poll after a WRITE)
 *     all       the defaults
 *     verify    polled payload reads, checksums verified (the blind gate's control run)
 *     blind     blind payload reads, checksums verified (the blind gate)
 *     fast      burst 3, hold 100 (hw3: cuts TCP send; for experiments only)
 *     quiet     one poll every 30 s: keeps MoofPort off the bus while ScsiProbe runs
 * Any other id without a cfg changes nothing and only reports.
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
#include <stdio.h>
#include <string.h>
#include "runid.h"
#include "mp_drv.h"

QDGlobals qd;

#pragma pack(push, 2)
typedef struct EPB {
    uint32_t qLink; int16_t qType; int16_t ioTrap; uint32_t ioCmdAddr; uint32_t ioCompletion;
    volatile int16_t ioResult; uint32_t ioNamePtr; int16_t ioVRefNum; int16_t ioRefNum;
    int16_t csCode; int16_t eProtType; uint32_t ePointer; int16_t eBuffSize; int16_t eDataSize;
    int16_t spare[10];
} EPB;
#pragma pack(pop)

static short g_outref = -1;
static Str255 g_outname;
static char g_line[300];
static void wr(const char *s) { long n = (long)strlen(s); if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s); }
#define LOG(...) do { sprintf(g_line, __VA_ARGS__); wr(g_line); } while (0)

static short g_ref;

static int get_stats(MpStats *st)
{
    EPB pb;
    memset(&pb, 0, sizeof pb);
    memset(st, 0, sizeof *st);
    pb.ioRefNum = g_ref;
    pb.csCode = MP_STATUS_STATS;
    pb.ePointer = (uint32_t)st;
    pb.eBuffSize = (int16_t)sizeof *st;
    return (PBStatusSync((ParmBlkPtr)&pb) == noErr && st->version == MP_STATS_VERSION) ? 0 : -1;
}

static void show(const char *when, const MpStats *st)
{
    const MpTune *t = &st->tune;
    LOG("%s: burst %u ms, hold %u, mid %u until %u, idle %u, step %u, busy %u; %s, %s, %s; "
        "verify %s\r", when, t->burst_ms, t->hold_ms, t->mid_ms, t->quiet_ms, t->idle_ms,
        t->step_ms, t->busy_ms, t->blind ? "BLIND payload reads" : "polled reads",
        t->blind_tx ? "BLIND frame writes" : "polled writes",
        (t->flags & MP_TUNE_NOBATCH) ? "one frame per WRITE" : "batched WRITEs",
        t->verify ? "on" : "off");
    LOG("  received frames per tick: at most %d; after a WRITE: poll at %d ms, reply chase %s\r",
        (t->flags & MP_TUNE_RX4) ? 4 : 2, (t->flags & MP_TUNE_ACK2) ? 2 : 5,
        (t->flags & MP_TUNE_NOCHASE) ? "off" : "on");
    LOG("  after a READ of replies only: %s; EWrite with nothing queued: %s; hush %d ms\r",
        (t->flags & MP_TUNE_NOYIELD) ? "keep reading" : "yield",
        (t->flags & MP_TUNE_NOINLINE) ? "queue" : t->early == MP_EARLY_1 ?
        "write at once (inline), chase the reply every 1 ms" : t->early == MP_EARLY_OFF ?
        "write at once (inline), chase at the usual 5 ms" : "write at once (inline), chase the reply every 2 ms",
        (t->flags & MP_TUNE_HUSH) ? 15 : 0);
    LOG("  after the reply to our own WRITE: %s; after a READ the yield cut short: %s\r",
        (t->flags2 & MP_TUNE2_NOPAUSE) ? "poll as usual" : "pause up to 12 ms",
        (t->flags2 & MP_TUNE2_REPOLL) ? "re-poll in 1 ms" : "the usual interval");
    LOG("  polls %lu, burst polls %lu, immediate %lu, kicks %lu, busy %lu, bus errors %lu\r",
        st->polls, st->bursts, st->immediates, st->kicks, st->busy, st->bus_err);
    LOG("  rx frames %lu, aborted %lu, bad %lu; verify ok %lu (1,000+ bytes %lu), bad %lu\r",
        st->rx.frames, st->rx.aborted, st->rx.bad, st->v_ok, st->v_big, st->v_bad);
}

static long num(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

static int set_key(MpTune *t, const char *k, long v)
{
    if (!strcmp(k, "burst")) t->burst_ms = (uint16_t)v;
    else if (!strcmp(k, "hold")) t->hold_ms = (uint16_t)v;
    else if (!strcmp(k, "mid")) t->mid_ms = (uint16_t)v;
    else if (!strcmp(k, "quiet")) t->quiet_ms = (uint16_t)v;
    else if (!strcmp(k, "idle")) t->idle_ms = (uint16_t)v;
    else if (!strcmp(k, "step")) t->step_ms = (uint16_t)v;
    else if (!strcmp(k, "busy")) t->busy_ms = (uint16_t)v;
    else if (!strcmp(k, "blind")) t->blind = (uint8_t)(v != 0);
    else if (!strcmp(k, "blind_tx")) t->blind_tx = (uint8_t)(v != 0);
    else if (!strcmp(k, "verify")) t->verify = (uint8_t)(v != 0);
    else if (!strcmp(k, "batch"))
        t->flags = (uint8_t)(v ? t->flags & ~MP_TUNE_NOBATCH : t->flags | MP_TUNE_NOBATCH);
    else if (!strcmp(k, "rx"))
        t->flags = (uint8_t)(v == 4 ? t->flags | MP_TUNE_RX4 : t->flags & ~MP_TUNE_RX4);
    else if (!strcmp(k, "ack"))
        t->flags = (uint8_t)(v == 2 ? t->flags | MP_TUNE_ACK2 : t->flags & ~MP_TUNE_ACK2);
    else if (!strcmp(k, "yield"))
        t->flags = (uint8_t)(v ? t->flags & ~MP_TUNE_NOYIELD : t->flags | MP_TUNE_NOYIELD);
    else if (!strcmp(k, "inline"))
        t->flags = (uint8_t)(v ? t->flags & ~MP_TUNE_NOINLINE : t->flags | MP_TUNE_NOINLINE);
    else if (!strcmp(k, "spin"))        /* diagnostic driver, Snow only: ms after an inline WRITE */
        t->pad = (uint8_t)v;
    else if (!strcmp(k, "pause"))
        t->flags2 = (uint8_t)(v ? t->flags2 & ~MP_TUNE2_NOPAUSE : t->flags2 | MP_TUNE2_NOPAUSE);
    else if (!strcmp(k, "repoll"))
        t->flags2 = (uint8_t)(v ? t->flags2 | MP_TUNE2_REPOLL : t->flags2 & ~MP_TUNE2_REPOLL);
    else if (!strcmp(k, "early"))       /* ms: 1, 2, or 0 = the usual poll after a WRITE */
        t->early = (uint8_t)(v == 1 ? MP_EARLY_1 : v == 2 ? MP_EARLY_2 : MP_EARLY_OFF);
    else if (!strcmp(k, "hush"))
        t->flags = (uint8_t)(v ? t->flags | MP_TUNE_HUSH : t->flags & ~MP_TUNE_HUSH);
    else if (!strcmp(k, "chase"))
        t->flags = (uint8_t)(v ? t->flags & ~MP_TUNE_NOCHASE : t->flags | MP_TUNE_NOCHASE);
    else return -1;
    return 0;
}

/* MoofTune.cfg: returns 1 if it was there. */
static int read_cfg(MpTune *t)
{
    short r;
    long len = 511, i = 0;
    static char buf[512];
    if (FSOpen((ConstStr255Param)"\pMoofTune.cfg", 0, &r) != noErr) return 0;
    if (FSRead(r, &len, (Ptr)buf) != noErr && len <= 0) len = 0;
    FSClose(r);
    buf[len] = 0;
    while (i < len) {
        char *l = buf + i, *eq;
        long e = i;
        while (e < len && buf[e] != '\r' && buf[e] != '\n') e++;
        buf[e] = 0;
        if ((eq = strchr(l, '=')) != 0) {
            *eq = 0;
            if (set_key(t, l, num(eq + 1)))
                LOG("MoofTune.cfg: unknown key \"%s\" ignored\r", l);
        }
        i = e + 1;
    }
    return 1;
}

static void defaults(MpTune *t)
{
    t->burst_ms = 5; t->hold_ms = 60; t->mid_ms = 17; t->quiet_ms = 2000; t->idle_ms = 51;
    t->step_ms = 17; t->busy_ms = 2;
    t->blind = 1; t->verify = 0; t->blind_tx = 1; t->flags = 0; t->early = MP_EARLY_OFF; t->pad = 0;
    t->flags2 = 0; t->pad2 = 0;
}

/* One poll per tick, always (0.3d3's timing). */
static void tick(MpTune *t)
{
    t->burst_ms = 17; t->hold_ms = 0; t->mid_ms = 0; t->quiet_ms = 0; t->idle_ms = 17;
    t->step_ms = 0;
}

/* The run id names a preset. Returns 1 if it is one. */
static int preset(MpTune *t, const unsigned char *outname)
{
    char id[32];
    int n = outname[0] - 9 - 4;           /* "MoofTune-" ... ".out" */
    if (n <= 0 || n >= (int)sizeof id) return 0;
    memcpy(id, outname + 1 + 9, (size_t)n);
    id[n] = 0;
    if (!strcmp(id, "default")) { defaults(t); return 1; }
    if (!strcmp(id, "tick")) { defaults(t); tick(t); return 1; }
    if (!strcmp(id, "phase1")) { defaults(t); tick(t); t->blind = 0; return 1; }
    if (!strcmp(id, "burst")) { defaults(t); tick(t); t->burst_ms = 5; t->hold_ms = 60; return 1; }
    if (!strcmp(id, "idle")) {
        defaults(t); tick(t);
        t->hold_ms = 2000; t->idle_ms = 51; t->step_ms = 17;
        return 1;
    }
    if (!strcmp(id, "txblind")) { t->blind_tx = 1; return 1; }
    if (!strcmp(id, "txpolled")) { t->blind_tx = 0; return 1; }
    if (!strcmp(id, "nobatch")) { defaults(t); t->flags |= MP_TUNE_NOBATCH; return 1; }
    if (!strcmp(id, "batch")) { defaults(t); return 1; }
    if (!strcmp(id, "rx2")) { defaults(t); return 1; }
    if (!strcmp(id, "rx4")) { defaults(t); t->flags |= MP_TUNE_RX4; return 1; }
    if (!strcmp(id, "inline")) { defaults(t); return 1; }
    if (!strcmp(id, "noinline")) { defaults(t); t->flags |= MP_TUNE_NOINLINE; return 1; }
    if (!strcmp(id, "chase1") || !strcmp(id, "early1")) { defaults(t); t->early = MP_EARLY_1; return 1; }
    if (!strcmp(id, "chase2") || !strcmp(id, "early2")) { defaults(t); t->early = MP_EARLY_2; return 1; }
    if (!strcmp(id, "inhushc1")) { defaults(t); t->flags |= MP_TUNE_HUSH; t->early = MP_EARLY_1; return 1; }
    if (!strcmp(id, "inhush0")) { defaults(t); t->flags |= MP_TUNE_HUSH; t->early = MP_EARLY_OFF; return 1; }
    if (!strcmp(id, "early0")) { defaults(t); t->early = MP_EARLY_OFF; return 1; }
    if (!strcmp(id, "inack2")) { defaults(t); t->flags |= MP_TUNE_ACK2; return 1; }
    if (!strcmp(id, "hush")) { defaults(t); t->flags |= MP_TUNE_NOINLINE | MP_TUNE_HUSH; return 1; }
    if (!strcmp(id, "inhush")) { defaults(t); t->flags |= MP_TUNE_HUSH; t->early = MP_EARLY_2; return 1; }
    if (!strcmp(id, "nopause")) { defaults(t); t->flags2 = MP_TUNE2_NOPAUSE; return 1; }
    if (!strcmp(id, "steady") || !strcmp(id, "steadytx")) {
        /* DaynaPORT's rhythm (hwsend12): a poll every 16 ms at every level, one frame per READ,
         * no yield cut, no chase, no pause, no early poll after an inline WRITE; steadytx also
         * with handshaked frame writes */
        defaults(t);
        t->burst_ms = t->mid_ms = t->idle_ms = 16; t->step_ms = 0;
        t->flags = MP_TUNE_NOYIELD | MP_TUNE_NOCHASE;
        t->flags2 = MP_TUNE2_NOPAUSE | MP_TUNE2_STEADY;
        if (id[6]) t->blind_tx = 0;
        return 1;
    }
    if (!strcmp(id, "newtx")) { defaults(t); t->blind_tx = 0; return 1; }
    if (!strcmp(id, "repoll")) { defaults(t); t->flags2 = MP_TUNE2_REPOLL; return 1; }
    if (!strcmp(id, "old10")) {      /* hwsend10's defaults: inline + chase2, no pause, re-poll */
        defaults(t); t->early = MP_EARLY_2; t->flags2 = MP_TUNE2_NOPAUSE | MP_TUNE2_REPOLL; return 1;
    }
    if (!strcmp(id, "oldhush")) {    /* hwsend10's inline + hush (h10, inhush0) */
        defaults(t); t->flags |= MP_TUNE_HUSH; t->flags2 = MP_TUNE2_NOPAUSE | MP_TUNE2_REPOLL; return 1;
    }
    if (!strcmp(id, "ack5")) { defaults(t); return 1; }
    if (!strcmp(id, "ack2")) { defaults(t); t->flags |= MP_TUNE_ACK2; return 1; }
    if (!strcmp(id, "noyield")) { defaults(t); t->flags |= MP_TUNE_NOYIELD; return 1; }
    if (!strcmp(id, "nochase")) { defaults(t); t->flags |= MP_TUNE_NOCHASE; return 1; }
    if (!strcmp(id, "all")) { defaults(t); return 1; }
    if (!strcmp(id, "quiet")) {
        t->burst_ms = 30000; t->hold_ms = 0; t->mid_ms = 0; t->idle_ms = 30000; t->busy_ms = 2000;
        return 1;
    }
    if (!strcmp(id, "verify")) { t->blind = 0; t->verify = 1; return 1; }
    if (!strcmp(id, "blind")) { t->blind = 1; t->verify = 1; return 1; }
    if (!strcmp(id, "fast")) { t->burst_ms = 3; t->hold_ms = 100; return 1; }
    return 0;
}

int main(void)
{
    EPB pb;
    MpStats st;
    MpTune t;
    Str255 ps;
    size_t n;
    const char *result = "nothing changed";

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
    runid_name("MoofTune", ".out", g_outname);
    FSDelete(g_outname, 0);
    Create(g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen(g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
    wr("MoofTune\r");

    if (OpenDriver((ConstStr255Param)"\p.ENET0", &g_ref) != noErr || get_stats(&st)) {
        LOG(".ENET0 is not MoofPort 0.3 or later (no tuning)\r");
        result = "the driver is not MoofPort 0.3";
        goto fin;
    }
    show("before", &st);
    t = st.tune;
    t.version = MP_TUNE_VERSION;
    if (read_cfg(&t)) wr("settings from MoofTune.cfg\r");
    else if (preset(&t, g_outname)) wr("settings from the preset named by the run id\r");
    else { wr("no MoofTune.cfg and the run id is not a preset: nothing changed\r"); goto fin; }

    memset(&pb, 0, sizeof pb);
    pb.ioRefNum = g_ref;
    pb.csCode = MP_CTL_TUNE;
    pb.ePointer = (uint32_t)&t;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
        LOG("MP_CTL_TUNE failed, %d\r", (int)pb.ioResult);
        result = "the driver refused the settings";
        goto fin;
    }
    if (!get_stats(&st)) show("after", &st);
    result = "settings applied until the next restart";
fin:
    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);
    sprintf(g_line, "MoofTune: %s. See %.*s", result, (int)g_outname[0], (const char *)g_outname + 1);
    n = strlen(g_line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, g_line, n);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

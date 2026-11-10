/* moofstat.c - MoofStat: print the MoofPort driver's counters (private Status call, mp_drv.h)
 * to an alert and to MoofStat-<run id>.out next to the app. */
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
#include "mp_diag.h"

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
static char g_line[400];
static void wr(const char *s) { long n = (long)strlen(s); if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s); }
#define LOG(...) do { sprintf(g_line, __VA_ARGS__); wr(g_line); } while (0)

int main(void)
{
    EPB pb;
    MpStats st;
    short ref = 0;
    OSErr r;
    Str255 ps;
    size_t n;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
    runid_name("MoofStat", ".out", g_outname);
    FSDelete(g_outname, 0);
    Create(g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen(g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
    wr("MoofStat\r");

    r = OpenDriver((ConstStr255Param)"\p.ENET0", &ref);
    if (r) { LOG("OpenDriver(.ENET0) = %d\r", (int)r); goto fin; }
    memset(&pb, 0, sizeof pb);
    memset(&st, 0, sizeof st);
    pb.ioRefNum = ref;
    pb.csCode = MP_STATUS_STATS;
    pb.ePointer = (uint32_t)&st;
    pb.eBuffSize = (int16_t)sizeof st;
    r = PBStatusSync((ParmBlkPtr)&pb);
    if (r || st.version != MP_STATS_VERSION) {
        LOG("refnum %d: not MoofPort (Status = %d)\r", (int)ref, (int)r);
        goto fin;
    }
    LOG("MoofPort refnum %d, SCSI ID %d, open %d, MAC %02x:%02x:%02x:%02x:%02x:%02x\r",
        (int)ref, (int)st.scsi_id, (int)st.opened, st.mac[0], st.mac[1], st.mac[2], st.mac[3],
        st.mac[4], st.mac[5]);
    LOG("polls %lu  busy %lu  bus errors %lu (last %d)  delivered %lu\r",
        st.polls, st.busy, st.bus_err, (int)st.last_err, st.delivered);
    LOG("rx: reads %lu empty %lu frames %lu multi %lu overflow %lu drained %lu aborted %lu bad %lu oversize %lu more_ended %lu\r",
        st.rx.polls, st.rx.empty, st.rx.frames, st.rx.multi, st.rx.overflow, st.rx.drained,
        st.rx.aborted, st.rx.bad, st.rx.oversize, st.rx.more_ended);
    LOG("tx: now %lu deferred %lu failed %lu | core sent %lu queued %lu full %lu badlen %lu writes %lu batches %lu errors %lu\r",
        st.tx_now, st.tx_deferred, st.tx_failed, st.tx.sent, st.tx.queued, st.tx.full,
        st.tx.badlen, st.tx.writes, st.tx.batches, st.tx.errors);
    LOG("demux: unattached %lu runts %lu\r", st.unattached, st.runts);
    LOG("poll-path: reply pause %s, re-poll after a yield cut %s\r",
        (st.tune.flags2 & MP_TUNE2_NOPAUSE) ? "off" : "12 ms", (st.tune.flags2 & MP_TUNE2_REPOLL) ? "on" : "off");
    LOG("watchdog: poll task re-armed %lu times\r", (unsigned long)st.wd);
    LOG("poll: burst %u ms, hold %u, mid %u until %u, idle %u, step %u, busy %u; kicks %lu, "
        "burst polls %lu, immediate %lu\r", st.tune.burst_ms, st.tune.hold_ms, st.tune.mid_ms,
        st.tune.quiet_ms, st.tune.idle_ms, st.tune.step_ms, st.tune.busy_ms, st.kicks, st.bursts,
        st.immediates);
    LOG("send-fix: %s, rx %d frames per tick, poll %d ms after a WRITE, reply chase %s, %s, %s, hush %d ms\r",
        (st.tune.flags & MP_TUNE_NOBATCH) ? "one frame per WRITE" : "batched WRITEs",
        (st.tune.flags & MP_TUNE_RX4) ? 4 : 2, (st.tune.flags & MP_TUNE_ACK2) ? 2 : 5,
        (st.tune.flags & MP_TUNE_NOCHASE) ? "off" : "on",
        (st.tune.flags & MP_TUNE_NOYIELD) ? "no yield" : "yield after replies",
        (st.tune.flags & MP_TUNE_NOINLINE) ? "queued writes" : st.tune.early == MP_EARLY_1 ?
        "inline writes, chase 1 ms" : st.tune.early == MP_EARLY_OFF ?
        "inline writes, chase 5 ms" : "inline writes, chase 2 ms",
        (st.tune.flags & MP_TUNE_HUSH) ? 15 : 0);
    LOG("reads: %s; writes: %s; verify %s: ok %lu (1,000+ bytes %lu), bad %lu\r",
        st.tune.blind ? "BLIND payload" : "polled", st.tune.blind_tx ? "BLIND frame" : "polled",
        st.tune.verify ? "on" : "off", st.v_ok,
        st.v_big, st.v_bad);
    {
        static MpLog lg;
        int i;
        static const char *const names[10] = { "EDelMulti", "EAddMulti", "EAttachPH", "EDetachPH",
            "EWrite", "ERead", "ERdCancel", "EGetInfo", "ESetGeneral", "other" };
        memset(&pb, 0, sizeof pb);
        pb.ioRefNum = ref;
        pb.csCode = MP_STATUS_LOG;
        pb.ePointer = (uint32_t)&lg;
        pb.eBuffSize = (int16_t)sizeof lg;
        if (PBStatusSync((ParmBlkPtr)&pb) == noErr && lg.version == MP_LOG_VERSION) {
            for (i = 0; i < 10; i++)
                if (lg.c.calls[i])
                    LOG("calls: %s %lu\r", names[i], lg.c.calls[i]);
            LOG("EWrites of 802.3 (length-field) frames: %lu\r", lg.c.write8023);
            for (i = 0; i < 8; i++)
                if (lg.ph[i].used)
                    LOG("handler: type 0x%04x %s, %lu frames\r", lg.ph[i].type,
                        lg.ph[i].has_handler ? "attached" : "ERead (no handler)", lg.ph[i].frames);
        }
    }
    {
        /* the hwsend diagnostic driver only: EWrites by kind since its last reset (TcpSend) */
        static MpDiagStats dg;
        memset(&pb, 0, sizeof pb);
        pb.ioRefNum = ref;
        pb.csCode = MP_STATUS_DIAG;
        pb.ePointer = (uint32_t)&dg;
        pb.eBuffSize = (int16_t)sizeof dg;
        if (PBStatusSync((ParmBlkPtr)&pb) == noErr && dg.version == MP_DIAG_VERSION)
            LOG("diag: EWrite kinds (task / interrupt level): async %lu / %lu, sync %lu / %lu, "
                "immediate %lu / %lu; sync left pending %lu; refused %lu\r", dg.ew_kind[0],
                dg.ew_kind[1], dg.ew_kind[2], dg.ew_kind[3], dg.ew_kind[4], dg.ew_kind[5],
                dg.ew_sync_park, dg.ew_refused);
    }
fin:
    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);
    sprintf(g_line, "MoofStat finished. See %.*s", (int)g_outname[0], (const char *)g_outname + 1);
    n = strlen(g_line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, g_line, n);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

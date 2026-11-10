/* tcpsend.c - TcpSend: TcpBench's echo and bulk send, in several send modes, timed per chunk.
 *
 * The hwsend kit (docs/dev/hwsend.md). TcpBench's bulk send (sync 1400-byte TCPSends with push)
 * takes about one echo round trip per chunk, and a little longer over MoofPort than over
 * DaynaPORT. TcpSend repeats it and two other ways of sending, and records each chunk:
 *
 *   E  echo     64 x 1 byte to the echo port 17007, each round trip timed
 *   A  sync1400 256 KB to the sink 17031 as sync 1400-byte TCPSends, push on each (TcpBench)
 *   B  sync8k   256 KB as sync 8,192-byte TCPSends, push only on the last
 *   C  async4   256 KB as 1400-byte TCPSends with push, four outstanding at a time
 *   N  nostamp  A again with the diagnostic driver's clock reads off (what they cost); only
 *               with the hwsend diagnostic driver
 *   R  recv     1 MB from the bulk source 17030 (TcpBench's receive), with the tick loss
 * With the diagnostic driver every mode also lists the frames the driver wrote (the frame log,
 * mp_diag.h): "W time type id seq len port rc index/frames [b]" per frame, for
 * scripts/hwsend-lost.py to match against the host capture.
 *
 * Every mode is its own TCP connection, so the host capture (scripts/hwsend-host.sh) can tell
 * them apart; they run in the order above. Per chunk: the time the TCPSend took (A, B), or the
 * time between completions (C), in microseconds from the revised Time Manager used as a
 * stopwatch (no Microseconds trap on 6.0.8; same method as ScsiProbe, about 250 us per read on
 * the Classic, measured at start and subtracted).
 *
 * With the hwsend diagnostic MoofPort driver (MoofPortDiag) each mode also gets the driver's
 * timing counters (mp_diag.h: nested vs app EWrites, delivery -> nested EWrite return, ...),
 * reset before the mode, and the change of the usual counters (writes, batches, polls, kicks).
 * With DaynaPORT or the shipping MoofPort those lines say the driver has no diagnostics.
 *
 * Settings: "TcpSend.cfg" (else "TcpBench.cfg"), first line "a.b.c.d [modes]", e.g.
 * "192.0.2.10 EABCRN" (the default modes). Results: TcpSend-<run id>.out.
 *
 * The MacTCP interface is TcpBench's (docs/dev/MACTCP-INTERFACE.md offsets, no Apple header).
 *
 * MoofBatch (the same source built with MP_BATCH, hwsend11): one run id, then every MoofTune
 * setting in "MoofBatch.cfg"'s second line ("settings new oldhush early0 noinline", the
 * default) in turn, set through the driver's MP_CTL_TUNE with no restart, and the modes for each
 * (default EACRAC). With DaynaPORT (no MoofPort counters) it runs the modes once as "dayna".
 * Before and after each test it sends "MARK <run> <setting> <n> <test> start|end" by UDP to the
 * PC's port 17040 (testnet.py logs it; scripts/hwsend-batch.py splits the capture by it). A
 * small window shows the setting, test N of M and the last result; it is redrawn only between
 * tests. Results: MoofBatch-<run id>.out. */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Memory.h>
#include <Files.h>
#include <Devices.h>
#include <Events.h>
#include <OSUtils.h>
#include <Timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "runid.h"
#include "mp_drv.h"
#include "mp_diag.h"

QDGlobals qd;

enum {
    CS_CREATE = 30, CS_ACTIVEOPEN = 32, CS_SEND = 34, CS_RCV = 37,
    CS_CLOSE = 38, CS_ABORT = 39, CS_RELEASE = 42
};

#define HOST_IP     0xAC110001u   /* 172.17.0.1 (Snow NAT host) */
#define ECHO_PORT   17007
#define BULK_SINK   17031
#define BULK_SRC    17030
#define connectionClosing (-23005)
#define commandTimeout    (-23016)
#define RCVBUF_LEN  8192
#define N_ECHO      64
#define TOTAL       262144u
#define SMALL       1400
#define BIG         8192
#define N_ASYNC     4
#define MAX_REC     200           /* 262144 / 1400 = 188 chunks */

#pragma pack(push, 2)
typedef struct BenchPB {
    uint32_t qLink; int16_t qType; int16_t ioTrap; uint32_t ioCmdAddr; uint32_t ioCompletion;
    volatile int16_t ioResult; uint32_t ioNamePtr; int16_t ioVRefNum; int16_t ioCRefNum;
    int16_t csCode; uint32_t tcpStream; uint8_t csParam[70];
} BenchPB;
typedef struct WDS { uint16_t length; uint32_t ptr; } WDS;
typedef struct EPB {
    uint32_t qLink; int16_t qType; int16_t ioTrap; uint32_t ioCmdAddr; uint32_t ioCompletion;
    volatile int16_t ioResult; uint32_t ioNamePtr; int16_t ioVRefNum; int16_t ioRefNum;
    int16_t csCode; int16_t eProtType; uint32_t ePointer; int16_t eBuffSize; int16_t eDataSize;
    int16_t spare[10];
} EPB;
#pragma pack(pop)

static void p8(BenchPB *pb, int off, uint8_t v) { pb->csParam[off] = v; }
static void p16(BenchPB *pb, int off, uint16_t v)
{ pb->csParam[off] = (uint8_t)(v >> 8); pb->csParam[off + 1] = (uint8_t)v; }
static void p32(BenchPB *pb, int off, uint32_t v)
{ pb->csParam[off] = (uint8_t)(v >> 24); pb->csParam[off + 1] = (uint8_t)(v >> 16);
  pb->csParam[off + 2] = (uint8_t)(v >> 8); pb->csParam[off + 3] = (uint8_t)v; }

static short g_ipp, g_enet;
static uint32_t g_stream;
static uint8_t g_strbuf[RCVBUF_LEN];
static uint8_t g_rcvbuf[64];
static uint8_t g_data[BIG];
static uint32_t g_host = HOST_IP;
#ifdef MP_BATCH
static char g_modes[16] = "EACRAC";
#else
static char g_modes[16] = "EABCRN";
#endif
static short g_outref = -1;
static Str255 g_outname;
static char g_line[256];
static int g_diag;                /* the hwsend diagnostic driver answered */
static int g_mp;                  /* MoofPort's counters (MP_STATUS_STATS) answered */
static unsigned long g_tk0;       /* Ticks and the stopwatch at the start of a mode */
static uint32_t g_us0;
static uint32_t g_rec[MAX_REC];
static int g_nrec;

static unsigned long ticks(void) { return *(volatile unsigned long *)0x16A; }
static void wr(const char *s) { long n = (long)strlen(s); if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s); }
#define LOG(...) do { sprintf(g_line, __VA_ARGS__); wr(g_line); } while (0)

/* ---- the stopwatch (revised Time Manager, see the top) ---- */

typedef struct Clock { TMTask tm; uint32_t acc; } Clock;   /* tm first: the entry gets it in A1 */
#define CLK_US 1000000L
extern void ts_clk_entry(void);         /* tcpsend_tm.s */
static Clock g_clk;
static uint32_t g_lap;                  /* cost of one read, subtracted from every lap */

void ts_clk_fire(Clock *c);
void ts_clk_fire(Clock *c)
{
    c->acc += (uint32_t)CLK_US;
    PrimeTime((QElemPtr)&c->tm, -CLK_US);
}

static uint32_t now_us(void)
{
    uint16_t sr;
    uint32_t t;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    RmvTime((QElemPtr)&g_clk.tm);
    g_clk.acc += (uint32_t)(CLK_US + g_clk.tm.tmCount);
    t = g_clk.acc;
    g_clk.tm.tmCount = 0;
    InsTime((QElemPtr)&g_clk.tm);
    PrimeTime((QElemPtr)&g_clk.tm, -CLK_US);
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
    return t;
}

static void clk_start(void)
{
    uint32_t a, b;
    int i;
    memset(&g_clk, 0, sizeof g_clk);
    g_clk.tm.tmAddr = (ProcPtr)ts_clk_entry;
    InsTime((QElemPtr)&g_clk.tm);
    PrimeTime((QElemPtr)&g_clk.tm, -CLK_US);
    g_lap = 0xFFFFFFFFu;
    for (i = 0; i < 16; i++) {
        a = now_us();
        b = now_us();
        if (b - a < g_lap) g_lap = b - a;
    }
}

static void clk_stop(void) { RmvTime((QElemPtr)&g_clk.tm); }

static void rec(uint32_t t0, uint32_t t1)
{
    uint32_t d = t1 - t0;
    d = d > g_lap ? d - g_lap : 0;
    if (g_nrec < MAX_REC) g_rec[g_nrec++] = d;
}

/* ---- MacTCP ---- */

static int16_t call(BenchPB *pb, int16_t csCode)
{
    pb->ioCRefNum = g_ipp;
    pb->csCode = csCode;
    pb->ioCompletion = 0;
    pb->ioResult = 1;
    return (int16_t)PBControlSync((ParmBlkPtr)pb);
}

static int16_t tcp_create(void)
{
    BenchPB pb;
    int16_t r;
    memset(&pb, 0, sizeof pb);
    p32(&pb, 0, (uint32_t)(uintptr_t)g_strbuf);
    p32(&pb, 4, (uint32_t)RCVBUF_LEN);
    r = call(&pb, CS_CREATE);
    if (r == 0) g_stream = pb.tcpStream;
    return r;
}

static int16_t tcp_open(uint16_t port)
{
    BenchPB pb;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    p8(&pb, 0, 30); p8(&pb, 1, 1); p8(&pb, 2, 0xC0);
    p32(&pb, 4, g_host);
    p16(&pb, 8, port);
    return call(&pb, CS_ACTIVEOPEN);
}

/* A TCPSend PB for len bytes at data (wds must stay valid until it completes). */
static void send_pb(BenchPB *pb, WDS *wds, const uint8_t *data, uint16_t len, int push)
{
    wds[0].length = len; wds[0].ptr = (uint32_t)(uintptr_t)data;
    wds[1].length = 0;   wds[1].ptr = 0;
    memset(pb, 0, sizeof *pb);
    pb->tcpStream = g_stream;
    p8(pb, 0, 30); p8(pb, 1, 1); p8(pb, 2, 0xC0);
    p8(pb, 3, push ? 1 : 0);
    p32(pb, 6, (uint32_t)(uintptr_t)wds);
}

static int16_t tcp_send(const uint8_t *data, uint16_t len, int push)
{
    BenchPB pb;
    WDS wds[2];
    send_pb(&pb, wds, data, len, push);
    return call(&pb, CS_SEND);
}

static int16_t tcp_rcv(uint16_t *got)
{
    BenchPB pb;
    int16_t r;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    p8(&pb, 0, 10);
    p32(&pb, 4, (uint32_t)(uintptr_t)g_rcvbuf);
    p16(&pb, 8, (uint16_t)sizeof g_rcvbuf);
    r = call(&pb, CS_RCV);
    *got = (uint16_t)((pb.csParam[8] << 8) | pb.csParam[9]);
    return r;
}

static void tcp_end(int abort)
{
    BenchPB pb;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    if (!abort) { p8(&pb, 0, 10); p8(&pb, 1, 1); p8(&pb, 2, 0xC0); }
    (void)call(&pb, abort ? CS_ABORT : CS_CLOSE);
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    (void)call(&pb, CS_RELEASE);
    g_stream = 0;
}

static int connect(const char *what, uint16_t port)
{
    int16_t r;
    int tries;
    for (tries = 0; tries < 2; tries++) {
        unsigned long t;
        if (tcp_create()) { LOG("%s: TCPCreate failed\r", what); return -1; }
        r = tcp_open(port);
        if (!r) return 0;
        LOG("%s: TCPActiveOpen -> %d%s\r", what, (int)r, tries ? "" : ", trying again in 1 s");
        tcp_end(1);
        /* Snow's NAT now and then refuses an open right after the last connection closed
         * (-23015, openFailed): one more try a second later. */
        for (t = ticks(); ticks() - t < 60; ) ;
    }
    return -1;
}

/* ---- the driver's counters ---- */

static MpStats g_s0, g_s1;
static MpDiagStats g_dg;
static MpWLog g_wl;
static uint8_t g_bigbuf[RCVBUF_LEN];

static int enet_status(int16_t cs, void *buf, int16_t len)
{
    EPB pb;
    memset(&pb, 0, sizeof pb);
    pb.ioRefNum = g_enet;
    pb.csCode = cs;
    pb.ePointer = (uint32_t)(uintptr_t)buf;
    pb.eBuffSize = len;
    return PBStatusSync((ParmBlkPtr)&pb) == noErr;
}

static void diag_ctl(int reset, int stamps)
{
    EPB pb;
    MpDiagCtl c;
    if (!g_diag) return;
    c.version = MP_DIAG_VERSION;
    c.op = reset ? MP_DIAG_OP_RESET : MP_DIAG_OP_NONE;
    c.stamps = (uint8_t)stamps;
    memset(&pb, 0, sizeof pb);
    pb.ioRefNum = g_enet;
    pb.csCode = MP_CTL_DIAG;
    pb.ePointer = (uint32_t)(uintptr_t)&c;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) LOG("diag control failed\r");
}

static void probe_driver(void)
{
    g_enet = 0;
    if (OpenDriver((ConstStr255Param)"\p.ENET0", &g_enet)) { LOG("driver: .ENET0 not open\r"); return; }
    memset(&g_dg, 0, sizeof g_dg);
    if (enet_status(MP_STATUS_DIAG, &g_dg, (int16_t)sizeof g_dg) && g_dg.version == MP_DIAG_VERSION) {
        g_diag = g_mp = 1;
        LOG("driver: MoofPort hwsend diagnostic, clock read %lu us\r", (unsigned long)g_dg.lap_us);
    } else if (enet_status(MP_STATUS_STATS, &g_s0, (int16_t)sizeof g_s0) &&
               g_s0.version == MP_STATS_VERSION) {
        g_mp = 1;
        LOG("driver: MoofPort (no diagnostics: not the hwsend build)\r");
    } else {
        LOG("driver: not MoofPort (DaynaPORT?): no driver counters\r");
    }
}

static void mode_begin(int stamps)
{
    diag_ctl(1, stamps);
    if (g_mp) enet_status(MP_STATUS_STATS, &g_s0, (int16_t)sizeof g_s0);
    g_tk0 = ticks();
    g_us0 = now_us();
}

/* Ticks against the stopwatch over the mode: ticks the 60.15 Hz VBL interrupt did not count
 * because interrupts were held off too long (the Time Manager stopwatch is not affected). */
static void tick_loss(void)
{
    uint32_t ms = (now_us() - g_us0) / 1000u;
    unsigned long got = ticks() - g_tk0;
    unsigned long want = (unsigned long)(ms * 6015u / 100000u);
    LOG("  ticks: %lu counted, %lu by the stopwatch (%lu ms): %ld lost (%ld%%)\r", got, want,
        (unsigned long)ms, (long)want - (long)got,
        want ? ((long)want - (long)got) * 100 / (long)want : 0);
}

static void print_hist(const char *name, const MpDiagHist *h)
{
    char *p = g_line;
    int i;
    if (!h->n) { LOG("  %-22s none\r", name); return; }
    p += sprintf(p, "  %-22s n %lu mean %lu us max %lu us |", name, (unsigned long)h->n,
                 (unsigned long)(h->sum_us / h->n), (unsigned long)h->max_us);
    for (i = 0; i < MP_DIAG_BINS; i++)
        if (h->bin[i]) p += sprintf(p, " %d:%lu", i, (unsigned long)h->bin[i]);
    strcpy(p, "\r");
    wr(g_line);
}

/* The send cycle (diag version 6): mean us of each stage, per app EWrite. */
static unsigned long mean(int k) { const MpDiagHist *h = &g_dg.h[k]; return h->n ? h->sum_us / h->n : 0; }
static void per(char *p, uint32_t v, uint32_t n)   /* v / n with one decimal */
{
    uint32_t x = n ? (v * 10 + n / 2) / n : 0;
    sprintf(p, "%lu.%lu", (unsigned long)(x / 10), (unsigned long)(x % 10));
}

static void print_cycle(void)
{
    uint32_t n = g_dg.h[MP_DH_C_TOTAL].n;
    char a[6][16];
    if (!n) { LOG("  send cycle: none\r"); return; }
    LOG("  send cycle (one per round trip: app EWrite -> WRITE -> reply delivered -> our tick returns "
        "-> next app EWrite; n %lu; mean us, stage n in brackets):\r", (unsigned long)n);
    LOG("    EWrite->WRITE start %lu [%lu], WRITE %lu [%lu], WRITE end->delivering tick %lu [%lu], "
        "tick start->handler %lu [%lu]\r", mean(MP_DH_C_EW_W), (unsigned long)g_dg.h[MP_DH_C_EW_W].n,
        mean(MP_DH_C_WRITE), (unsigned long)g_dg.h[MP_DH_C_WRITE].n, mean(MP_DH_C_W_READ),
        (unsigned long)g_dg.h[MP_DH_C_W_READ].n, mean(MP_DH_C_READ), (unsigned long)g_dg.h[MP_DH_C_READ].n);
    LOG("    handler %lu [%lu], handler ret->tick ret %lu [%lu], tick ret->next EWrite %lu [%lu], "
        "total %lu\r", mean(MP_DH_C_HANDLER), (unsigned long)g_dg.h[MP_DH_C_HANDLER].n,
        mean(MP_DH_C_TICKRET), (unsigned long)g_dg.h[MP_DH_C_TICKRET].n, mean(MP_DH_C_MACTCP),
        (unsigned long)g_dg.h[MP_DH_C_MACTCP].n, mean(MP_DH_C_TOTAL));
    per(a[0], g_dg.c_reads, n); per(a[1], g_dg.c_reads_post, n); per(a[2], g_dg.c_ticks, n);
    per(a[3], g_dg.c_ticks_post, n); per(a[4], g_dg.c_clk, n); per(a[5], g_dg.c_writes, n);
    LOG("    our CPU (ticks + EWrite calls) %lu, of it after the delivering tick %lu; per cycle: "
        "WRITEs %s, READs %s (after %s), ticks %s (after %s), clock reads %s at %lu us; VBL runs %lu, "
        "app EWrites beyond the first %lu (all cycles)\r", mean(MP_DH_C_CPU), mean(MP_DH_C_CPU_POST),
        a[5], a[0], a[1], a[2], a[3], a[4], (unsigned long)g_dg.lap_us, (unsigned long)g_dg.c_vbl,
        (unsigned long)g_dg.c_ew_more);
    print_hist("cycle total", &g_dg.h[MP_DH_C_TOTAL]);
    print_hist("cycle our CPU", &g_dg.h[MP_DH_C_CPU]);
    print_hist("tick ret->next EWrite", &g_dg.h[MP_DH_C_MACTCP]);
}

static void mode_end(void)
{
    tick_loss();
    if (!g_mp) return;
    if (!enet_status(MP_STATUS_STATS, &g_s1, (int16_t)sizeof g_s1)) {
        LOG("  driver counters: Status failed\r");
        return;
    }
#define D(f) (unsigned long)(g_s1.f - g_s0.f)
    LOG("  driver: polls %lu, kicks %lu, delivered %lu, tx now %lu queued %lu failed %lu, "
        "WRITEs %lu batches %lu, READs %lu empty %lu, busy %lu, immediate polls %lu, watchdog %lu\r",
        D(polls), D(kicks), D(delivered), D(tx_now), D(tx_deferred), D(tx_failed), D(tx.writes),
        D(tx.batches), D(rx.polls), D(rx.empty), D(busy), D(immediates), D(wd));
#undef D
    if (!g_diag) return;
    memset(&g_dg, 0, sizeof g_dg);
    if (!enet_status(MP_STATUS_DIAG, &g_dg, (int16_t)sizeof g_dg)) {
        LOG("  diag counters: Status failed\r");
        return;
    }
    LOG("  diag: stamps %s, ticks %lu (%lu delivered), frames %lu (%lu wrote back), "
        "clock reads %lu fires %lu\r", g_dg.stamps ? "on" : "OFF", (unsigned long)g_dg.ticks,
        (unsigned long)g_dg.ticks_rx, (unsigned long)g_dg.delivered, (unsigned long)g_dg.deliv_ew,
        (unsigned long)g_dg.clk_reads, (unsigned long)g_dg.clk_fires);
    LOG("  diag: EWrite nested %lu (queued %lu, pending %lu), app %lu (queued %lu, pending %lu, "
        "at interrupt level %lu), other-in-tick %lu, errors %lu\r", (unsigned long)g_dg.ew_nested,
        (unsigned long)g_dg.ew_nested_q, (unsigned long)g_dg.ew_nested_park,
        (unsigned long)g_dg.ew_app, (unsigned long)g_dg.ew_app_q, (unsigned long)g_dg.ew_app_park,
        (unsigned long)g_dg.ew_app_irq,
        (unsigned long)g_dg.ew_tick_other, (unsigned long)g_dg.ew_err);
    LOG("  diag: EWrite kinds (task / interrupt level): async %lu / %lu, sync %lu / %lu, "
        "immediate %lu / %lu; sync left pending %lu; refused %lu\r",
        (unsigned long)g_dg.ew_kind[0], (unsigned long)g_dg.ew_kind[1], (unsigned long)g_dg.ew_kind[2],
        (unsigned long)g_dg.ew_kind[3], (unsigned long)g_dg.ew_kind[4], (unsigned long)g_dg.ew_kind[5],
        (unsigned long)g_dg.ew_sync_park, (unsigned long)g_dg.ew_refused);
    if (g_dg.stamps) {
        LOG("  diag histograms (us; bins k:n, bin k = k*1.024 ms up to (k+1)*1.024, 19 = 19.5 ms and up)\r");
        print_hist("deliver->nested ret", &g_dg.h[MP_DH_DELIV_RET]);
        print_hist("nested EWrite", &g_dg.h[MP_DH_NEST_EW]);
        print_hist("app EWrite", &g_dg.h[MP_DH_APP_EW]);
        print_hist("app EWrite->next tick", &g_dg.h[MP_DH_KICK]);
        print_hist("poll tick", &g_dg.h[MP_DH_TICK]);
        print_hist("handler ret->app EWrite", &g_dg.h[MP_DH_ACK_EW]);
        print_hist("WRITE begin", &g_dg.h[MP_DH_W_BEGIN]);
        print_hist("WRITE data", &g_dg.h[MP_DH_W_DATA]);
        print_hist("WRITE end", &g_dg.h[MP_DH_W_END]);
        print_hist("VBL->handler ret", &g_dg.h[MP_DH_PH_DELIV]);
        print_hist("VBL->app EWrite", &g_dg.h[MP_DH_PH_EW]);
        print_hist("WRITE end->reply", &g_dg.h[MP_DH_REPLY]);
        print_cycle();
    }
    memset(&g_wl, 0, sizeof g_wl);
    if (enet_status(MP_STATUS_WLOG, &g_wl, (int16_t)sizeof g_wl) && g_wl.version == MP_DIAG_VERSION) {
        uint32_t n = g_wl.total < MP_WLOG_N ? g_wl.total : MP_WLOG_N, i;
        uint16_t at = (uint16_t)(g_wl.total < MP_WLOG_N ? 0 : g_wl.pos);
        LOG("  frame log: %lu frames written, the last %lu below (W time type id seq len port rc i/n [b])\r",
            (unsigned long)g_wl.total, (unsigned long)n);
        for (i = 0; i < n; i++) {
            const MpWEntry *e = &g_wl.e[(at + i) & (MP_WLOG_N - 1)];
            LOG("W %u %04x %u %lu %u %u %d %u/%u%s\r", e->t, e->etype, e->id, (unsigned long)e->seq,
                e->len, e->port, (int)e->rc, e->pos & 7, (e->pos >> 3) & 15,
                (e->pos & 0x80) ? " b" : "");
        }
    }
}

/* ---- per-chunk report ---- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* g_rec: the raw list (ms, one decimal), then min / median / mean / max and a histogram in
 * bins of `bin_ms`. */
static void report(const char *what, int bin_ms)
{
    static uint32_t v[MAX_REC];
    uint32_t sum = 0;
    int i, b, n = g_nrec;
    char *p;
    if (!n) { LOG("  %s: no chunks recorded\r", what); return; }
    LOG("  %s per chunk, ms in order (clock read %lu us subtracted):\r", what, (unsigned long)g_lap);
    for (i = 0; i < n; i += 10) {
        int j;
        p = g_line;
        p += sprintf(p, "   ");
        for (j = i; j < n && j < i + 10; j++)
            p += sprintf(p, " %lu.%lu", (unsigned long)(g_rec[j] / 1000), (unsigned long)(g_rec[j] % 1000 / 100));
        strcpy(p, "\r");
        wr(g_line);
    }
    memcpy(v, g_rec, sizeof v[0] * (size_t)n);
    qsort(v, (size_t)n, sizeof v[0], cmp_u32);
    for (i = 0; i < n; i++) sum += v[i];
    LOG("  %s: %d chunks, min %lu us, median %lu, mean %lu, max %lu\r", what, n,
        (unsigned long)v[0], (unsigned long)v[n / 2], (unsigned long)(sum / (uint32_t)n),
        (unsigned long)v[n - 1]);
    p = g_line;
    p += sprintf(p, "  histogram (%d ms bins, lower edge:count):", bin_ms);
    for (i = 0; i < n; ) {
        uint32_t lo = v[i] / (1000u * (uint32_t)bin_ms);
        int c = 0;
        while (i < n && v[i] / (1000u * (uint32_t)bin_ms) == lo) { c++; i++; }
        b = (int)lo * bin_ms;
        if (p - g_line > 200) { strcpy(p, "\r"); wr(g_line); p = g_line; p += sprintf(p, "   "); }
        p += sprintf(p, " %d:%d", b, c);
    }
    strcpy(p, "\r");
    wr(g_line);
}

static char g_last[64];             /* the last result, for MoofBatch's window */
static void rate(const char *what, uint32_t bytes, unsigned long dt)
{
    sprintf(g_last, "%s: %lu B/s", what, dt ? (unsigned long)((uint32_t)((uint64_t)bytes * 60 / dt)) : 0);
    LOG("%s: %lu bytes in %lu ticks (%lu B/s)\r", what, (unsigned long)bytes, dt,
        dt ? (unsigned long)((uint32_t)((uint64_t)bytes * 60 / dt)) : 0);
}

/* ---- the modes ---- */

static void mode_echo(void)
{
    int i, lost = 0;
    LOG("-- E echo: %d x 1 byte to %d\r", N_ECHO, ECHO_PORT);
    if (connect("echo", ECHO_PORT)) return;
    mode_begin(1);
    g_nrec = 0;
    for (i = 0; i < N_ECHO; i++) {
        uint8_t b = (uint8_t)('A' + (i & 31));
        uint16_t got = 0;
        uint32_t t0 = now_us();
        if (tcp_send(&b, 1, 1)) { lost++; continue; }
        if (tcp_rcv(&got) == 0 && got >= 1) rec(t0, now_us());
        else lost++;
    }
    LOG("echo: %d ok, %d lost\r", g_nrec, lost);
    sprintf(g_last, "echo: %d ok, %d lost", g_nrec, lost);
    report("echo RTT", 2);
    mode_end();
    tcp_end(0);
}

/* Sync sends of `chunk` bytes; push on each, or (push_last) only on the last. */
static void mode_sync(const char *name, uint16_t chunk, int push_last, int stamps, int bin_ms)
{
    uint32_t sent = 0;
    unsigned long t0;
    LOG("-- %s: %lu bytes to %d, sync %u-byte TCPSends, push %s%s\r", name,
        (unsigned long)TOTAL, BULK_SINK, chunk, push_last ? "on the last only" : "on each",
        stamps ? "" : ", driver clock reads OFF");
    if (connect(name, BULK_SINK)) return;
    mode_begin(stamps);
    g_nrec = 0;
    t0 = ticks();
    while (sent < TOTAL) {
        uint16_t n = (uint16_t)((TOTAL - sent) < chunk ? (TOTAL - sent) : chunk);
        uint32_t c0 = now_us();
        int16_t r = tcp_send(g_data, n, !push_last || sent + n >= TOTAL);
        rec(c0, now_us());
        if (r) { LOG("TCPSend -> %d after %lu bytes\r", (int)r, (unsigned long)sent); break; }
        sent += n;
    }
    rate(name, sent, ticks() - t0);
    report(name, bin_ms);
    mode_end();
    diag_ctl(0, 1);
    tcp_end(0);
}

/* N_ASYNC sends of SMALL bytes outstanding; a new one goes as soon as the oldest completes. */
static void mode_async(void)
{
    static BenchPB pb[N_ASYNC];
    static WDS wds[N_ASYNC][2];
    uint32_t queued = 0, done = 0, last;
    unsigned long t0;
    int i, k = 0, err = 0;
    LOG("-- C async%d: %lu bytes to %d, %d-byte TCPSends with push, %d outstanding\r", N_ASYNC,
        (unsigned long)TOTAL, BULK_SINK, SMALL, N_ASYNC);
    if (connect("async", BULK_SINK)) return;
    mode_begin(1);
    g_nrec = 0;
    t0 = ticks();
    last = now_us();
    for (i = 0; i < N_ASYNC && queued < TOTAL; i++) {
        uint16_t n = (uint16_t)((TOTAL - queued) < SMALL ? (TOTAL - queued) : SMALL);
        send_pb(&pb[i], wds[i], g_data, n, 1);
        pb[i].ioCRefNum = g_ipp; pb[i].csCode = CS_SEND; pb[i].ioResult = 1;
        PBControlAsync((ParmBlkPtr)&pb[i]);
        queued += n;
    }
    while (done < queued) {
        uint32_t t;
        while (pb[k].ioResult > 0) ;
        t = now_us();
        rec(last, t);
        last = t;
        if (pb[k].ioResult != 0 && !err) {
            err = pb[k].ioResult;
            LOG("TCPSend -> %d after %lu bytes\r", err, (unsigned long)done);
        }
        done += wds[k][0].length;
        if (!err && queued < TOTAL) {
            uint16_t n = (uint16_t)((TOTAL - queued) < SMALL ? (TOTAL - queued) : SMALL);
            send_pb(&pb[k], wds[k], g_data, n, 1);
            pb[k].ioCRefNum = g_ipp; pb[k].csCode = CS_SEND; pb[k].ioResult = 1;
            PBControlAsync((ParmBlkPtr)&pb[k]);
            queued += n;
        }
        k = (k + 1) % N_ASYNC;
    }
    rate("async", done, ticks() - t0);
    report("async (between completions)", 2);
    mode_end();
    tcp_end(err != 0);
}

static void mode_recv(void)
{
    BenchPB pb;
    uint32_t total = 0;
    unsigned long t0;
    int16_t r = 0;
    LOG("-- R recv: from the bulk source %d until it closes\r", BULK_SRC);
    if (connect("recv", BULK_SRC)) return;
    mode_begin(1);
    t0 = ticks();
    for (;;) {
        uint16_t got;
        memset(&pb, 0, sizeof pb);
        pb.tcpStream = g_stream;
        p8(&pb, 0, 20);
        p32(&pb, 4, (uint32_t)(uintptr_t)g_bigbuf);
        p16(&pb, 8, (uint16_t)RCVBUF_LEN);
        r = call(&pb, CS_RCV);
        got = (uint16_t)((pb.csParam[8] << 8) | pb.csParam[9]);
        if (r == 0) total += got;
        if (r == connectionClosing) break;
        if (r != 0 && r != commandTimeout) break;
        if (r == commandTimeout && got == 0) break;
    }
    rate("R recv", total, ticks() - t0);
    LOG("  ended by %d\r", (int)r);
    mode_end();
    tcp_end(0);
}

#ifdef MP_BATCH
/* ---- MoofBatch: settings, markers, progress ---- */

static char g_settings[128] = "new oldhush early0 noinline";
static char g_run[32];
static uint32_t g_ustream;
static uint8_t g_ubuf[2048];
static WindowPtr g_win;

enum { CS_UCREATE = 20, CS_UWRITE = 23, CS_URELEASE = 24 };
#define MARK_PORT 17040

static void mark(const char *setting, int n, char test, const char *what)
{
    BenchPB pb;
    WDS wds[2];
    static char msg[96];
    if (!g_ustream) return;
    sprintf(msg, "MARK %s %s %d %c %s", g_run[0] ? g_run : "-", setting, n, test, what);
    wds[0].length = (uint16_t)strlen(msg); wds[0].ptr = (uint32_t)(uintptr_t)msg;
    wds[1].length = 0; wds[1].ptr = 0;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_ustream;
    p32(&pb, 2, g_host);
    p16(&pb, 6, MARK_PORT);
    p32(&pb, 8, (uint32_t)(uintptr_t)wds);
    p8(&pb, 12, 1);
    (void)call(&pb, CS_UWRITE);
}

static void udp_open(void)
{
    BenchPB pb;
    memset(&pb, 0, sizeof pb);
    p32(&pb, 0, (uint32_t)(uintptr_t)g_ubuf);
    p32(&pb, 4, (uint32_t)sizeof g_ubuf);
    if (call(&pb, CS_UCREATE) == 0) g_ustream = pb.tcpStream;
    else LOG("markers: UDPCreate failed, no markers\r");
}

static void udp_close(void)
{
    BenchPB pb;
    if (!g_ustream) return;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_ustream;
    (void)call(&pb, CS_URELEASE);
    g_ustream = 0;
}

/* Set a named setting through MP_CTL_TUNE: MoofTune's defaults, then the setting's changes. */
static int apply_setting(const char *setting)
{
    EPB pb;
    MpTune t;
    char name[24];
    int polled = 0;
    size_t n = strlen(setting);
    /* "<name>-txpolled": the same with handshaked frame writes (blind_tx off, hwsend12) */
    if (n > 9 && !strcmp(setting + n - 9, "-txpolled")) { polled = 1; n -= 9; }
    if (n >= sizeof name) return -2;
    memcpy(name, setting, n);
    name[n] = 0;
    if (!enet_status(MP_STATUS_STATS, &g_s0, (int16_t)sizeof g_s0)) return -1;
    t = g_s0.tune;
    t.version = MP_TUNE_VERSION;
    t.burst_ms = 5; t.hold_ms = 60; t.mid_ms = 17; t.quiet_ms = 2000; t.idle_ms = 51;
    t.step_ms = 17; t.busy_ms = 2;
    t.blind = 1; t.verify = 0; t.blind_tx = 1; t.flags = 0; t.early = MP_EARLY_OFF;
    t.pad = 0; t.flags2 = 0; t.pad2 = 0;
    if (!strcmp(name, "new")) {
        /* the hwsend11 defaults: inline, first poll 5 ms after it, reply pause, no re-poll */
    } else if (!strcmp(name, "oldhush")) {
        t.flags = MP_TUNE_HUSH; t.flags2 = MP_TUNE2_NOPAUSE | MP_TUNE2_REPOLL;
    } else if (!strcmp(name, "early0")) {
        t.flags2 = MP_TUNE2_NOPAUSE | MP_TUNE2_REPOLL;
    } else if (!strcmp(name, "noinline")) {
        t.flags = MP_TUNE_NOINLINE; t.flags2 = MP_TUNE2_NOPAUSE | MP_TUNE2_REPOLL;
    } else if (!strcmp(name, "old10")) {
        t.early = MP_EARLY_2; t.flags2 = MP_TUNE2_NOPAUSE | MP_TUNE2_REPOLL;
    } else if (!strcmp(name, "nopause")) {
        t.flags2 = MP_TUNE2_NOPAUSE;
    } else if (!strcmp(name, "repoll")) {
        t.flags2 = MP_TUNE2_REPOLL;
    } else if (!strcmp(name, "steady")) {
        /* DaynaPORT's rhythm (MoofTune steady): a poll every 16 ms at every level, one frame per
         * READ, no yield cut, chase, pause or early poll after an inline WRITE */
        t.burst_ms = t.mid_ms = t.idle_ms = 16; t.step_ms = 0;
        t.flags = MP_TUNE_NOYIELD | MP_TUNE_NOCHASE;
        t.flags2 = MP_TUNE2_NOPAUSE | MP_TUNE2_STEADY;
    } else {
        return -2;
    }
    if (polled) t.blind_tx = 0;
    memset(&pb, 0, sizeof pb);
    pb.ioRefNum = g_enet;
    pb.csCode = MP_CTL_TUNE;
    pb.ePointer = (uint32_t)(uintptr_t)&t;
    return PBControlSync((ParmBlkPtr)&pb) == noErr ? 0 : -3;
}

static void cline(const char *s)
{
    unsigned char ps[100];
    size_t n = strlen(s);
    if (n > 99) n = 99;
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, s, n);
    DrawString(ps);
}

/* Redrawn only between tests, never while one is timed. */
static void progress(const char *setting, int n, char test, int k, int total)
{
    Rect r;
    char line[100];
    if (!g_win) return;
    SetPort(g_win);
    r = g_win->portRect;
    EraseRect(&r);
    MoveTo(10, 18);
    sprintf(line, "MoofBatch %s: test %d of %d", g_run, k, total);
    cline(line);
    MoveTo(10, 36);
    sprintf(line, "setting %s, %c #%d", setting, test, n);
    cline(line);
    MoveTo(10, 54);
    sprintf(line, "last: %s", g_last[0] ? g_last : "-");
    cline(line);
}

static void run_mode(char m)
{
    switch (m) {
    case 'E': mode_echo(); break;
    case 'A': mode_sync("A sync1400", SMALL, 0, 1, 2); break;
    case 'B': mode_sync("B sync8k", BIG, 1, 1, 20); break;
    case 'C': mode_async(); break;
    case 'R': mode_recv(); break;
    default: break;
    }
}

static void batch(void)
{
    char list[128], *names[10], *p;
    int ns = 0, s, i, k = 0, total, cnt[128];
    strcpy(list, g_mp ? g_settings : "dayna");
    for (p = strtok(list, " "); p && ns < 10; p = strtok(0, " ")) names[ns++] = p;
    total = ns * (int)strlen(g_modes);
    udp_open();
    for (s = 0; s < ns; s++) {
        LOG("== SETTING %s\r", names[s]);
        if (g_mp) {
            int r = apply_setting(names[s]);
            if (r) { LOG("setting %s: not applied (%d), skipped\r", names[s], r); k += (int)strlen(g_modes); continue; }
            enet_status(MP_STATUS_STATS, &g_s0, (int16_t)sizeof g_s0);
            LOG("tune: flags %02x early %u flags2 %02x blind_tx %u poll %u/%u/%u ms\r", g_s0.tune.flags,
                g_s0.tune.early, g_s0.tune.flags2, g_s0.tune.blind_tx, g_s0.tune.burst_ms,
                g_s0.tune.mid_ms, g_s0.tune.idle_ms);
        }
        memset(cnt, 0, sizeof cnt);
        for (i = 0; g_modes[i]; i++) {
            char m = g_modes[i];
            int n = ++cnt[(unsigned char)m & 127];
            progress(names[s], n, m, ++k, total);
            mark(names[s], n, m, "start");
            run_mode(m);
            mark(names[s], n, m, "end");
        }
    }
    udp_close();
}
#endif

static void read_cfg(void)
{
#ifdef MP_BATCH
    static const unsigned char *const names[2] = { "\pMoofBatch.cfg", "\pTcpSend.cfg" };
#else
    static const unsigned char *const names[2] = { "\pTcpSend.cfg", "\pTcpBench.cfg" };
#endif
    int i;
    for (i = 0; i < 2; i++) {
        short r = -1;
        long len = 0;
        char buf[160], m[16];
        int a, b, c, d, k;
        if (FSOpen((ConstStr255Param)names[i], 0, &r) != noErr) continue;
        GetEOF(r, &len);
        if (len > 159) len = 159;
        FSRead(r, &len, (Ptr)buf);
        FSClose(r);
        buf[len] = 0;
        k = sscanf(buf, "%d.%d.%d.%d %15s", &a, &b, &c, &d, m);
        if (k >= 4)
            g_host = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
        if (k == 5 && i == 0) strcpy(g_modes, m);
#ifdef MP_BATCH
        {
            char *l2 = strchr(buf, '\r');
            if (!l2) l2 = strchr(buf, '\n');
            if (l2 && i == 0 && !strncmp(l2 + 1, "settings ", 9)) {
                strncpy(g_settings, l2 + 10, sizeof g_settings - 1);
                g_settings[strcspn(g_settings, "\r\n")] = 0;
            }
        }
#endif
        LOG("settings from %.*s\r", (int)names[i][0], (const char *)names[i] + 1);
        return;
    }
}

int main(void)
{
    Str255 ps;
    size_t n;
    int i;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
#ifdef MP_BATCH
    runid_name("MoofBatch", ".out", g_outname);
    if (g_outname[0] > 10) {            /* "MoofBatch-<id>.out" -> <id> */
        int len = g_outname[0] - 10 - 4;
        if (len > 0 && len < (int)sizeof g_run) { memcpy(g_run, g_outname + 11, (size_t)len); g_run[len] = 0; }
    }
    {
        Rect wr = { 60, 20, 130, 380 };
        g_win = NewWindow(0, &wr, (ConstStr255Param)"\pMoofBatch", true, documentProc, (WindowPtr)-1, false, 0);
        if (g_win) { SetPort(g_win); TextFont(4); TextSize(9); }
    }
#else
    runid_name("TcpSend", ".out", g_outname);
#endif
    FSDelete((ConstStr255Param)g_outname, 0);
    Create((ConstStr255Param)g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStr255Param)g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
#ifdef MP_BATCH
    wr("MoofBatch (hwsend kit: every setting in one run, PC markers on UDP 17040)\r");
#else
    wr("TcpSend (hwsend kit: MacTCP send modes, per-chunk timing)\r");
#endif
    read_cfg();
    LOG("host %08lx, modes %s, start tick %lu\r", (unsigned long)g_host, g_modes, ticks());
    for (i = 0; i < (int)sizeof g_data; i++) g_data[i] = (uint8_t)i;

    if (OpenDriver((ConstStr255Param)"\p.IPP", &g_ipp)) { LOG("OpenDriver(.IPP) failed (is MacTCP installed?)\r"); goto fin; }
    probe_driver();
    clk_start();
    LOG("stopwatch: one read costs %lu us (Time Manager %s)\r", (unsigned long)g_lap,
        g_lap < 2000 ? "ok" : "SUSPECT");

#ifdef MP_BATCH
    LOG("settings %s\r", g_mp ? g_settings : "dayna (not MoofPort: one pass, no setting changes)");
    batch();
    if (0)
#endif
    for (i = 0; g_modes[i]; i++) {
        switch (g_modes[i]) {
        case 'E': mode_echo(); break;
        case 'A': mode_sync("A sync1400", SMALL, 0, 1, 2); break;
        case 'B': mode_sync("B sync8k", BIG, 1, 1, 20); break;
        case 'C': mode_async(); break;
        case 'R': mode_recv(); break;
        case 'N':
            if (g_diag) mode_sync("N sync1400 nostamp", SMALL, 0, 0, 2);
            else LOG("-- N skipped (no diagnostic driver)\r");
            break;
        default: break;
        }
    }
    clk_stop();
    LOG("end tick %lu\r", ticks());
    wr("RESULT DONE\r");
fin:
    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);
#ifdef MP_BATCH
    if (g_win) { DisposeWindow(g_win); g_win = 0; }
    sprintf(g_line, "MoofBatch finished. Results are in %.*s", (int)g_outname[0], (const char *)g_outname + 1);
#else
    sprintf(g_line, "TcpSend finished. Results are in %.*s", (int)g_outname[0], (const char *)g_outname + 1);
#endif
    n = strlen(g_line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, g_line, n);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

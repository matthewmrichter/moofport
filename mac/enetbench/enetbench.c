/* enetbench.c - EnetBench: a raw .ENET0 (DaynaPORT) frame benchmark.
 *
 * Phase 0 baseline kit (docs/dev/PLAN.md section 4, step 5): the raw-driver half of the baseline,
 * run with MacTCP CLOSED (we attach our own IP + ARP protocol handlers, which would clash
 * with MacTCP's -94 LAPProtErr). It opens the Ethernet driver directly, and measures the
 * driver, not the TCP stack. Model: MoofTCP's mac/enetspike/enetspike.c and mt_enet.c, but
 * self-contained (no portable core) and talking to whichever .ENET0 is installed.
 *
 * What it does (all timings in ticks, 60.15 Hz; results to EnetBench.out on the boot vol):
 *   - OpenDriver(".ENET0"), EGetInfo -> our 6-byte MAC.
 *   - EAttachPH the one handler (enetbench_ph.s) for 0x0806 (ARP) and 0x0800 (IP).
 *   - ARP for the gateway (default 10.0.0.1) to learn its MAC; ARP round-trip time.
 *   - sync EWrite time vs async EWrite time (single ARP request each).
 *   - frames/s each way: a run of N ARP request/reply round trips (receive rate = replies
 *     seen; send rate = EWrites completed).
 *   - ICMP echo to the gateway, RTT histogram in ticks over N echoes.
 *   - handler context: SR seen at entry, and stack pointer depth, read from the ring.
 *
 * Config: the gateway IP and our source IP default to Snow's (GW_IP / MY_IP). A text file
 * "EnetBench.cfg" next to the app overrides them; its first line is "gateway [our-address]",
 * both dotted, for example "192.168.1.1 192.168.1.50". A real router only answers an address
 * on its own subnet.
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
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "runid.h"
#include <stdint.h>

QDGlobals qd;

enum { E_ATTACH = 247, E_DETACH = 248, E_WRITE = 249, E_GETINFO = 252 };

#define GW_IP   0x0A000001u     /* 10.0.0.1 (Snow NAT gateway); override via EnetBench.cfg */
#define MY_IP   0x0A00004Du     /* 10.0.0.77, self-assigned (MacTCP is closed) */
#define N_RTT   64              /* echoes / ARP round trips for the rate + histogram runs */

/* ---- Ethernet PB (csParam at offset 28; size 58). From docs/dev/MACTCP-INTERFACE.md. ---- */
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
typedef struct WDS { uint16_t len; uint32_t ptr; } WDS;
#pragma pack(pop)

/* ---- the handler ring; layout mirrored by enetbench_ph.s (_Static_assert below) ---- */
typedef struct EbSlot {
    uint16_t len;        /* frame length stored (header included) */
    uint16_t etype;      /* EtherType */
    uint16_t sr;         /* SR at handler entry */
    uint16_t pad;
    uint32_t tick;       /* TickCount at entry */
    uint32_t sp;         /* stack pointer at entry (after the stub's movem, see .s) */
    uint8_t  buf[1520];
} EbSlot;
typedef struct EbRing {
    volatile uint16_t head, tail;
    volatile uint16_t drops;
    uint16_t nslots;
    EbSlot slot[8];
} EbRing;

#ifdef __mc68000__
_Static_assert(sizeof(EbSlot) == 1536, "EbSlot size");
_Static_assert(offsetof(EbSlot, len) == 0 && offsetof(EbSlot, etype) == 2 &&
               offsetof(EbSlot, sr) == 4 && offsetof(EbSlot, tick) == 8 &&
               offsetof(EbSlot, sp) == 12 && offsetof(EbSlot, buf) == 16, "EbSlot layout");
_Static_assert(offsetof(EbRing, head) == 0 && offsetof(EbRing, tail) == 2 &&
               offsetof(EbRing, drops) == 4 && offsetof(EbRing, nslots) == 6 &&
               offsetof(EbRing, slot) == 8, "EbRing layout");
_Static_assert(sizeof(((EbRing *)0)->slot) / sizeof(EbSlot) == 8, "8 slots");
#endif

extern void eb_ph(void);
extern void eb_ph_set_ring(void *ring);

/* ---- byte helpers (big-endian on the wire; no struct overlays for multi-byte fields) ---- */
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint16_t ones_sum(const uint8_t *p, int n, uint32_t acc)
{
    int i;
    for (i = 0; i + 1 < n; i += 2) acc += rd16(p + i);
    if (i < n) acc += (uint32_t)p[i] << 8;
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)acc;
}

/* ---- globals (the app) ---- */
static EbRing g_ring;
static short g_ref = 0;
static uint8_t g_mac[6];
static uint8_t g_gwmac[6];
static int g_have_gw = 0;
static uint32_t g_gw_ip = GW_IP;
static uint32_t g_my_ip = MY_IP;
static short g_outref = -1;
static Str255 g_outname;      /* "<Tool>-<run id>.out", see runid.c */
static uint8_t g_attached = 0;
static uint16_t g_seen_sr = 0;          /* SR values OR'd across all handler calls */
static uint32_t g_min_sp = 0xFFFFFFFFu; /* lowest handler SP seen (deepest stack) */
static uint32_t g_max_sp = 0;           /* highest handler SP seen */
static long g_frames_seen = 0;

static unsigned long ticks(void) { return *(volatile unsigned long *)0x16A; }

static void wr(const char *s)
{
    long n = (long)strlen(s);
    if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s);
}
static char g_line[256];
#define LOG(...) do { sprintf(g_line, __VA_ARGS__); wr(g_line); } while (0)

/* ---- driver calls ---- */
static int16_t ctl_sync(EPB *pb)
{
    pb->ioRefNum = g_ref;
    pb->ioCompletion = 0;
    return (int16_t)PBControlSync((ParmBlkPtr)pb);
}
static int16_t attach(uint16_t type)
{
    EPB pb;
    memset(&pb, 0, sizeof pb);
    pb.csCode = E_ATTACH;
    pb.eProtType = (int16_t)type;
    pb.ePointer = (uint32_t)(uintptr_t)eb_ph;
    return ctl_sync(&pb);
}
static void detach(uint16_t type)
{
    EPB pb;
    memset(&pb, 0, sizeof pb);
    pb.csCode = E_DETACH;
    pb.eProtType = (int16_t)type;
    (void)ctl_sync(&pb);
}

/* EWrite one frame (in `frame`, `len` bytes, already padded to >= 60); sync or async.
 * For async we spin on ioResult until complete and return elapsed ticks via *elapsed. */
static int16_t ewrite(const uint8_t *frame, uint16_t len, int async, long *elapsed)
{
    static EPB pb;          /* static so an async PB outlives this call's frame if needed */
    static WDS wds[2];
    unsigned long t0;
    int16_t r;
    wds[0].len = len;
    wds[0].ptr = (uint32_t)(uintptr_t)frame;
    wds[1].len = 0; wds[1].ptr = 0;
    memset(&pb, 0, sizeof pb);
    pb.csCode = E_WRITE;
    pb.ioRefNum = g_ref;
    pb.ePointer = (uint32_t)(uintptr_t)wds;
    t0 = ticks();
    if (async) {
        pb.ioCompletion = 0;
        r = (int16_t)PBControlAsync((ParmBlkPtr)&pb);
        if (r == 0) { while (pb.ioResult > 0) { } r = pb.ioResult; }
    } else {
        pb.ioCompletion = 0;
        r = (int16_t)PBControlSync((ParmBlkPtr)&pb);
    }
    if (elapsed) *elapsed = (long)(ticks() - t0);
    return r;
}

/* ---- frame builders ---- */
static uint16_t build_arp_request(uint8_t *f, uint32_t target_ip)
{
    int i;
    for (i = 0; i < 6; i++) f[i] = 0xFF;          /* dst = broadcast */
    memcpy(f + 6, g_mac, 6);                       /* src (driver overwrites, but fill) */
    wr16(f + 12, 0x0806);                          /* EtherType ARP */
    wr16(f + 14, 0x0001);                          /* HW type Ethernet */
    wr16(f + 16, 0x0800);                          /* proto IPv4 */
    f[18] = 6; f[19] = 4;                          /* HW/proto lengths */
    wr16(f + 20, 0x0001);                          /* op = request */
    memcpy(f + 22, g_mac, 6);                      /* sender HW */
    wr32(f + 28, g_my_ip);                           /* sender IP */
    memset(f + 32, 0, 6);                          /* target HW unknown */
    wr32(f + 38, target_ip);                       /* target IP */
    memset(f + 42, 0, 60 - 42);                    /* pad to 60 */
    return 60;
}

static uint16_t build_icmp_echo(uint8_t *f, uint16_t seq, uint16_t payload)
{
    uint8_t *ip = f + 14, *icmp = f + 14 + 20;
    uint16_t iplen = (uint16_t)(20 + 8 + payload);
    uint16_t i;
    memcpy(f, g_gwmac, 6);                          /* dst = gateway MAC */
    memcpy(f + 6, g_mac, 6);
    wr16(f + 12, 0x0800);                           /* IPv4 */
    ip[0] = 0x45; ip[1] = 0x00;
    wr16(ip + 2, iplen);
    wr16(ip + 4, seq);                              /* id */
    wr16(ip + 6, 0x0000);                           /* flags/frag */
    ip[8] = 64; ip[9] = 1;                          /* TTL, proto ICMP */
    wr16(ip + 10, 0);                               /* checksum (fill below) */
    wr32(ip + 12, g_my_ip);
    wr32(ip + 16, g_gw_ip);
    wr16(ip + 10, (uint16_t)~ones_sum(ip, 20, 0));
    icmp[0] = 8; icmp[1] = 0;                        /* echo request */
    wr16(icmp + 2, 0);                               /* checksum */
    wr16(icmp + 4, seq);                             /* id */
    wr16(icmp + 6, seq);                             /* seq */
    for (i = 0; i < payload; i++) icmp[8 + i] = (uint8_t)(i + 1);
    wr16(icmp + 2, (uint16_t)~ones_sum(icmp, (int)(8 + payload), 0));
    return (uint16_t)(14 + iplen);
}

/* Drain one ring slot; returns 1 and fills *out when a frame was present. */
static int ring_pop(EbSlot *out)
{
    uint16_t t = g_ring.tail;
    if (t == g_ring.head) return 0;
    *out = g_ring.slot[t];
    t = (uint16_t)((t + 1) % 8);
    g_ring.tail = t;
    g_seen_sr |= out->sr;
    if (out->sp < g_min_sp) g_min_sp = out->sp;
    if (out->sp > g_max_sp) g_max_sp = out->sp;
    g_frames_seen++;
    return 1;
}

/* Wait up to max_ticks for a frame matching EtherType etype; return slot or 0. */
static int wait_frame(EbSlot *out, uint16_t etype, unsigned long max_ticks)
{
    unsigned long t0 = ticks();
    while (ticks() - t0 < max_ticks) {
        while (ring_pop(out)) {
            if (etype == 0 || out->etype == etype) return 1;
        }
    }
    return 0;
}

static int do_arp(void)
{
    uint8_t f[64];
    EbSlot s;
    uint16_t len = build_arp_request(f, g_gw_ip);
    long el;
    unsigned long t0 = ticks();
    int16_t r = ewrite(f, len, 1, &el);
    if (r != 0) { LOG("ARP EWrite error %d\r", (int)r); return 0; }
    while (ticks() - t0 < 120) {
        if (!wait_frame(&s, 0x0806, 1)) continue;
        /* ARP reply op=2 with sender IP == gateway */
        if (s.len >= 42 && rd16(s.buf + 20) == 0x0002 && rd32(s.buf + 28) == g_gw_ip) {
            memcpy(g_gwmac, s.buf + 22, 6);
            g_have_gw = 1;
            LOG("ARP reply after %lu ticks; gateway %08lx MAC %02x:%02x:%02x:%02x:%02x:%02x\r",
                (unsigned long)(s.tick - t0), (unsigned long)g_gw_ip,
                g_gwmac[0], g_gwmac[1], g_gwmac[2], g_gwmac[3], g_gwmac[4], g_gwmac[5]);
            return 1;
        }
    }
    LOG("ARP: no reply within 2 s\r");
    return 0;
}

static void bench_ewrite_sync_vs_async(void)
{
    uint8_t f[64];
    uint16_t len = build_arp_request(f, g_gw_ip);
    long el_s = 0, el_a = 0;
    int i;
    long sum_s = 0, sum_a = 0;
    for (i = 0; i < 20; i++) { ewrite(f, len, 0, &el_s); sum_s += el_s; }
    for (i = 0; i < 20; i++) { ewrite(f, len, 1, &el_a); sum_a += el_a; }
    LOG("EWrite 20x sync: %ld ticks total (%ld.%02ld/call); 20x async(+wait): %ld ticks (%ld.%02ld/call)\r",
        sum_s, sum_s / 20, (sum_s * 100 / 20) % 100,
        sum_a, sum_a / 20, (sum_a * 100 / 20) % 100);
}

static void drain_ring(void)
{
    EbSlot s;
    while (ring_pop(&s)) { }
}

static void bench_rate_arp(void)
{
    uint8_t f[64];
    uint16_t len = build_arp_request(f, g_gw_ip);
    EbSlot s;
    int i, replies = 0;
    unsigned long t0, tx_ticks, first = 0, last = 0;
    uint32_t d0;
#define COUNT_REPLY() do { if (s.etype == 0x0806 && s.len >= 42 && rd16(s.buf + 20) == 0x0002) { \
        if (replies++ == 0) first = s.tick; \
        last = s.tick; } } while (0)
    { unsigned long t = ticks(); while (ticks() - t < 60) drain_ring(); }  /* leftovers from before */
    d0 = g_ring.drops;
    t0 = ticks();
    for (i = 0; i < N_RTT; i++) {
        ewrite(f, len, 1, 0);
        while (ring_pop(&s)) COUNT_REPLY();     /* keep the 8-slot ring drained while sending */
    }
    tx_ticks = ticks() - t0;
    /* collect replies for up to 2 s */
    t0 = ticks();
    while (ticks() - t0 < 120) {
        if (wait_frame(&s, 0x0806, 1)) COUNT_REPLY();
    }
#undef COUNT_REPLY
    LOG("rate: %d ARP requests sent in %lu ticks (%lu req/s); %d replies in %lu ticks (%lu frames/s, ring drops %lu)\r",
        N_RTT, tx_ticks, tx_ticks ? (unsigned long)((long)N_RTT * 60 / (long)tx_ticks) : 0,
        replies, last - first, last != first ? (unsigned long)((long)replies * 60 / (long)(last - first)) : 0,
        (unsigned long)(g_ring.drops - d0));
}

static void bench_echo_rtt(void)
{
    uint8_t f[128];
    EbSlot s;
    int i;
    long hist[8];           /* 0,1,2,3,4,5,6,7+ ticks */
    long rtts = 0, sum = 0, mx = 0, lost = 0;
    if (!g_have_gw) { LOG("echo RTT: skipped (no gateway MAC)\r"); return; }
    for (i = 0; i < 8; i++) hist[i] = 0;
    for (i = 0; i < N_RTT; i++) {
        uint16_t len = build_icmp_echo(f, (uint16_t)(1000 + i), 32);
        unsigned long t0 = ticks();
        int got = 0;
        ewrite(f, len, 1, 0);
        while (ticks() - t0 < 60) {
            if (wait_frame(&s, 0x0800, 1)) {
                /* ICMP echo reply: proto 1, type 0, id matches */
                if (s.len >= 14 + 20 + 8 && s.buf[14 + 9] == 1 &&
                    s.buf[14 + 20] == 0 && rd16(s.buf + 14 + 20 + 4) == (uint16_t)(1000 + i)) {
                    long rtt = (long)(s.tick - t0);
                    int b = rtt < 0 ? 0 : (rtt > 7 ? 7 : (int)rtt);
                    hist[b]++;
                    rtts++; sum += rtt; if (rtt > mx) mx = rtt;
                    got = 1;
                    break;
                }
            }
        }
        if (!got) lost++;
    }
    LOG("echo RTT: %ld replies, %ld lost; mean %ld.%02ld ticks, max %ld ticks\r",
        rtts, lost, rtts ? sum / rtts : 0, rtts ? (sum * 100 / rtts) % 100 : 0, mx);
    LOG("echo RTT histogram (ticks): 0=%ld 1=%ld 2=%ld 3=%ld 4=%ld 5=%ld 6=%ld 7+=%ld\r",
        hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
}

static void report_handler_context(void)
{
    /* g_seen_sr / g_min_sp / g_max_sp are accumulated in ring_pop over every frame drained
     * this run. The stub records SP after pushing 15 longs (60 bytes), so the true entry SP
     * is 60 bytes higher; the stack USED below entry is (entry SP range). */
    LOG("handler context: %ld frames seen; SR (OR of all entries) = 0x%04x (expect 0x2600,\r",
        g_frames_seen, (unsigned)g_seen_sr);
    LOG("  supervisor + mask 6 per docs/dev/MACTCP-INTERFACE.md); handler SP range 0x%08lx..0x%08lx\r",
        (unsigned long)g_min_sp, (unsigned long)g_max_sp);
    LOG("  (recorded 60 bytes below entry SP); ring drops this run = %lu\r",
        (unsigned long)g_ring.drops);
}

static void read_cfg(void)
{
    short r = -1;
    long len = 0;
    char buf[48];
    int a, b, c, d, e, f, g, h, n;
    if (FSOpen((ConstStr255Param)"\pEnetBench.cfg", 0, &r) != noErr) return;
    GetEOF(r, &len);
    if (len > 47) len = 47;
    if (FSRead(r, &len, (Ptr)buf) == noErr || len > 0) {
        buf[len] = 0;
        n = sscanf(buf, "%d.%d.%d.%d %d.%d.%d.%d", &a, &b, &c, &d, &e, &f, &g, &h);
        if (n >= 4)
            g_gw_ip = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
        if (n == 8)
            g_my_ip = ((uint32_t)e << 24) | ((uint32_t)f << 16) | ((uint32_t)g << 8) | (uint32_t)h;
    }
    FSClose(r);
}

int main(void)
{
    EPB pb;
    uint8_t info[18];
    short ref = 0;
    int16_t r;
    int i;
    Str255 ps;
    size_t n;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
    runid_name("EnetBench", ".out", g_outname);

    memset(&g_ring, 0, sizeof g_ring);
    g_ring.nslots = 8;

    FSDelete((ConstStr255Param)g_outname, 0);
    Create((ConstStr255Param)g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStr255Param)g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
    wr("EnetBench (raw .ENET0 benchmark; MacTCP must be closed)\r");

    read_cfg();
    LOG("gateway %08lx, our IP %08lx\r", (unsigned long)g_gw_ip, (unsigned long)g_my_ip);

    r = (int16_t)OpenDriver((ConstStr255Param)"\p.ENET0", &ref);
    if (r) { LOG("OpenDriver(.ENET0) failed: %d\r", (int)r); goto fin; }
    g_ref = ref;
    LOG("OpenDriver(.ENET0) ok, refnum %d\r", (int)ref);

    memset(&pb, 0, sizeof pb);
    memset(info, 0, sizeof info);
    pb.csCode = E_GETINFO;
    pb.ePointer = (uint32_t)(uintptr_t)info;
    pb.eBuffSize = (int16_t)sizeof info;
    r = ctl_sync(&pb);
    if (r) { LOG("EGetInfo failed: %d\r", (int)r); goto fin; }
    memcpy(g_mac, info, 6);
    LOG("our MAC %02x:%02x:%02x:%02x:%02x:%02x\r",
        g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);

    eb_ph_set_ring(&g_ring);
    r = attach(0x0806);
    if (r) { LOG("EAttachPH 0x0806 failed: %d (MacTCP still open?)\r", (int)r); goto fin; }
    g_attached |= 1;
    r = attach(0x0800);
    if (r) { LOG("EAttachPH 0x0800 failed: %d\r", (int)r); goto fin; }
    g_attached |= 2;

    do_arp();
    bench_ewrite_sync_vs_async();
    bench_rate_arp();
    bench_echo_rtt();
    report_handler_context();

fin:
    if (g_attached & 2) detach(0x0800);
    if (g_attached & 1) detach(0x0806);
    eb_ph_set_ring(0);
    g_attached = 0;
    /* Leave the driver open; closing .ENET0 under some systems is unneeded for a benchmark. */
    wr("RESULT DONE\r");
    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);

    (void)i;
    sprintf(g_line, "EnetBench finished. Results are in %.*s", (int)g_outname[0], (const char *)g_outname + 1);
    n = strlen(g_line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, g_line, n);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

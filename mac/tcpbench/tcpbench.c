/* tcpbench.c - TcpBench: TCP-level benchmark over MacTCP's .IPP driver.
 *
 * Phase 0 baseline kit (docs/dev/PLAN.md section 4, step 5): the MacTCP-level numbers. It drives the
 * .IPP driver directly with PBControl calls (asynchronous, yielding; see call()) and measures:
 *   - TCP connect time (TCPActiveOpen) in ticks,
 *   - echo RTT against testnet.py's echo port 17007 (send 1 byte, receive it back), and
 *   - bulk receive from the bulk source 17030 and bulk send to the bulk sink 17031, in B/s.
 * Results go to an alert and to TcpBench.out on the boot volume.
 *
 * The MacTCP interface is declared here from documented FACTS only (docs/dev/MACTCP-INTERFACE.md,
 * which was built against MoofTCP's own mac/include/mt_mactcp.h; NO Apple header is copied).
 * The PB is one struct for the fixed header (whose offsets are standard) plus a 70-byte
 * csParam area whose per-call fields are written by offset with byte helpers, so no overlay
 * cast depends on a guessed struct layout. Offsets are the authoritative numbers from that doc.
 *
 * Configurable host: constant HOST_IP (default 172.17.0.1, the Snow NAT host address). A
 * text file "TcpBench.cfg" whose first line is "a.b.c.d" overrides it.
 *
 * The csCodes, PB header offsets and per-call csParam
 * offsets are from MACTCP-INTERFACE.md; the driver-open/PBControl usage matches MoofTCP's
 * apps.
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
#include <Events.h>
#include <OSUtils.h>
#include <stdio.h>
#include <string.h>
#include "runid.h"
#include <stdint.h>

QDGlobals qd;

/* csCodes (ref, MACTCP-INTERFACE.md) */
enum {
    CS_CREATE = 30, CS_ACTIVEOPEN = 32, CS_SEND = 34, CS_RCV = 37,
    CS_CLOSE = 38, CS_ABORT = 39, CS_STATUS = 40, CS_RELEASE = 42
};

/* errors we test for (ref) */
#define connectionClosing (-23005)
#define commandTimeout    (-23016)

#define HOST_IP   0xAC110001u   /* 172.17.0.1 (Snow NAT host); override via TcpBench.cfg */
#define ECHO_PORT   17007
#define BULK_SRC    17030
#define BULK_SINK   17031
#define RCVBUF_LEN  8192        /* >= MacTCP's 4096 minimum */
#define N_ECHO      64
#define BULK_SEND_TOTAL 262144u /* bytes to push at the bulk sink */

/* The .IPP TCP parameter block: 32-byte header (standard offsets) + 70-byte csParam. */
#pragma pack(push, 2)
typedef struct BenchPB {
    uint32_t qLink;          /* 0 */
    int16_t  qType;          /* 4 */
    int16_t  ioTrap;         /* 6 */
    uint32_t ioCmdAddr;      /* 8  (0..11 = fill12, driver-owned) */
    uint32_t ioCompletion;   /* 12 */
    volatile int16_t ioResult; /* 16 */
    uint32_t ioNamePtr;      /* 18 (at 2 mod 4: this is why the PB packs to 2) */
    int16_t  ioVRefNum;      /* 22 */
    int16_t  ioCRefNum;      /* 24 (the .IPP driver refnum) */
    int16_t  csCode;         /* 26 */
    uint32_t tcpStream;      /* 28 */
    uint8_t  csParam[70];    /* 32..101 */
} BenchPB;                   /* size 102 */
typedef struct WDS { uint16_t length; uint32_t ptr; } WDS;  /* wdsEntry, 6 bytes */
typedef struct RDS { uint16_t length; uint32_t ptr; } RDS;  /* rdsEntry, 6 bytes */
#pragma pack(pop)

/* byte helpers for csParam multi-byte fields (big-endian / native longs as the PB stores them) */
static void p8(BenchPB *pb, int off, uint8_t v) { pb->csParam[off] = v; }
static void p16(BenchPB *pb, int off, uint16_t v)
{ pb->csParam[off] = (uint8_t)(v >> 8); pb->csParam[off + 1] = (uint8_t)v; }
static void p32(BenchPB *pb, int off, uint32_t v)
{ pb->csParam[off] = (uint8_t)(v >> 24); pb->csParam[off + 1] = (uint8_t)(v >> 16);
  pb->csParam[off + 2] = (uint8_t)(v >> 8); pb->csParam[off + 3] = (uint8_t)v; }
static uint32_t g32(BenchPB *pb, int off)
{ return ((uint32_t)pb->csParam[off] << 24) | ((uint32_t)pb->csParam[off + 1] << 16)
       | ((uint32_t)pb->csParam[off + 2] << 8) | pb->csParam[off + 3]; }

static short g_ipp = 0;          /* .IPP driver refnum */
static uint32_t g_stream = 0;    /* current TCP stream */
static uint8_t g_strbuf[RCVBUF_LEN];   /* MacTCP owns this for the life of the stream */
static uint8_t g_rcvbuf[RCVBUF_LEN];
static uint32_t g_host = HOST_IP;
static short g_outref = -1;
static Str255 g_outname;      /* "<Tool>-<run id>.out", see runid.c */
static char g_line[256];

static unsigned long ticks(void) { return *(volatile unsigned long *)0x16A; }
static void wr(const char *s) { long n = (long)strlen(s); if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s); }
#define LOG(...) do { sprintf(g_line, __VA_ARGS__); wr(g_line); } while (0)

/* Every MacTCP call goes through here. By default it is synchronous, as in hw1/hw2, so the
 * numbers compare across sessions. With the word "yield" in TcpBench.cfg (after the address,
 * e.g. "192.0.2.10 yield") it is issued asynchronously and waited for with WaitNextEvent
 * (sleep 0), so under MultiFinder the Finder can come forward and another application (AfpCopy)
 * can run beside it. Yielding costs measurable time (hw3: echo maxima of 80-150 ticks), so use
 * it only for side-by-side runs (or give a run id starting with "both"). */
static int g_yield;

/* A run id starting with "both" (e.g. "both", "both2") also turns yielding on: the side-by-side
 * step then needs no edited settings file. */
static int id_says_both(const unsigned char *outname, int toollen)
{
    return outname[0] >= toollen + 5 && !memcmp(outname + 1 + toollen + 1, "both", 4);
}

static void yield(void)
{
    EventRecord ev;
    WaitNextEvent(everyEvent, &ev, 0, 0);
}

static int16_t call(BenchPB *pb, int16_t csCode)
{
    pb->ioCRefNum = g_ipp;
    pb->csCode = csCode;
    pb->ioCompletion = 0;
    pb->ioResult = 1;
    if (!g_yield)
        return (int16_t)PBControlSync((ParmBlkPtr)pb);
    PBControlAsync((ParmBlkPtr)pb);
    while (pb->ioResult > 0)
        yield();
    return pb->ioResult;
}

static int16_t tcp_create(void)
{
    BenchPB pb;
    int16_t r;
    memset(&pb, 0, sizeof pb);
    /* TCPCreatePB (csParam): rcvBuff 0, rcvBuffLen 4, notifyProc 8, userDataPtr 12 */
    p32(&pb, 0, (uint32_t)(uintptr_t)g_strbuf);
    p32(&pb, 4, (uint32_t)RCVBUF_LEN);
    p32(&pb, 8, 0);            /* no notify proc: synchronous calls only */
    r = call(&pb, CS_CREATE);
    if (r == 0) g_stream = pb.tcpStream;
    return r;
}

static int16_t tcp_open(uint32_t ip, uint16_t port, long *elapsed_ticks)
{
    BenchPB pb;
    int16_t r;
    unsigned long t0;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    /* TCPOpenPB: ulpTimeoutValue 0, ulpTimeoutAction 1, validityFlags 2, commandTimeoutValue 3,
       remoteHost 4, remotePort 8, localHost 10, localPort 14, tosFlags 16, precedence 17,
       dontFrag 18, timeToLive 19 */
    p8(&pb, 0, 30);            /* ulpTimeout 30 s */
    p8(&pb, 1, 1);             /* action: abort on timeout */
    p8(&pb, 2, 0xC0);          /* validity: timeoutValue | timeoutAction */
    p8(&pb, 3, 0);
    p32(&pb, 4, ip);
    p16(&pb, 8, port);
    p32(&pb, 10, 0);           /* local host: any */
    p16(&pb, 14, 0);           /* local port: auto */
    t0 = ticks();
    r = call(&pb, CS_ACTIVEOPEN);
    if (elapsed_ticks) *elapsed_ticks = (long)(ticks() - t0);
    return r;
}

static int16_t tcp_send(const uint8_t *data, uint16_t len)
{
    BenchPB pb;
    WDS wds[2];
    wds[0].length = len; wds[0].ptr = (uint32_t)(uintptr_t)data;
    wds[1].length = 0;   wds[1].ptr = 0;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    /* TCPSendPB: ulpTimeoutValue 0, ulpTimeoutAction 1, validityFlags 2, pushFlag 3,
       urgentFlag 4, filler 5, wdsPtr 6 */
    p8(&pb, 0, 30); p8(&pb, 1, 1); p8(&pb, 2, 0xC0);
    p8(&pb, 3, 1);             /* push */
    p8(&pb, 4, 0);             /* not urgent */
    p32(&pb, 6, (uint32_t)(uintptr_t)wds);
    return call(&pb, CS_SEND);
}

/* TCPRcv into g_rcvbuf; returns the Device Manager result and sets *got to bytes read. */
static int16_t tcp_rcv(uint16_t *got, uint8_t cmdTimeout)
{
    BenchPB pb;
    int16_t r;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    /* TCPReceivePB: commandTimeoutValue 0, markFlag 1, urgentFlag 2, filler 3, rcvBuff 4,
       rcvBuffLen 8, rdsPtr 10, rdsLength 14, secondTimeStamp 16 */
    p8(&pb, 0, cmdTimeout);
    p32(&pb, 4, (uint32_t)(uintptr_t)g_rcvbuf);
    p16(&pb, 8, (uint16_t)RCVBUF_LEN);    /* rcvBuffLen in/out (bytes available/returned) */
    r = call(&pb, CS_RCV);
    if (got) *got = (uint16_t)((pb.csParam[8] << 8) | pb.csParam[9]);
    return r;
}

static void tcp_close_release(void)
{
    BenchPB pb;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    /* TCPClosePB: ulpTimeoutValue 0, ulpTimeoutAction 1, validityFlags 2 */
    p8(&pb, 0, 10); p8(&pb, 1, 1); p8(&pb, 2, 0xC0);
    (void)call(&pb, CS_CLOSE);
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    (void)call(&pb, CS_RELEASE);
    g_stream = 0;
}

static void tcp_abort_release(void)
{
    BenchPB pb;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    (void)call(&pb, CS_ABORT);
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = g_stream;
    (void)call(&pb, CS_RELEASE);
    g_stream = 0;
}

static void bench_connect_and_echo(void)
{
    long connect_ticks = 0;
    int16_t r;
    int i;
    long sum = 0, mx = 0, lost = 0, done = 0;

    if (tcp_create()) { LOG("echo: TCPCreate failed\r"); return; }
    r = tcp_open(g_host, ECHO_PORT, &connect_ticks);
    if (r) { LOG("echo: TCPActiveOpen -> %d\r", (int)r); tcp_abort_release(); return; }
    LOG("connect to %08lx:%d took %ld ticks (%ld ms)\r",
        (unsigned long)g_host, ECHO_PORT, connect_ticks, connect_ticks * 1000 / 60);

    for (i = 0; i < N_ECHO; i++) {
        uint8_t b = (uint8_t)('A' + (i & 31));
        uint16_t got = 0;
        unsigned long t0 = ticks();
        if (tcp_send(&b, 1)) { lost++; continue; }
        r = tcp_rcv(&got, 10);     /* 10 s command timeout */
        if (r == 0 && got >= 1) {
            long rtt = (long)(ticks() - t0);
            sum += rtt; if (rtt > mx) mx = rtt; done++;
        } else {
            lost++;
        }
    }
    LOG("echo RTT (port %d): %ld ok, %ld lost; mean %ld.%02ld ticks, max %ld ticks\r",
        ECHO_PORT, done, lost, done ? sum / done : 0, done ? (sum * 100 / done) % 100 : 0, mx);
    tcp_close_release();
}

static void bench_bulk_recv(void)
{
    long connect_ticks = 0;
    int16_t r;
    uint32_t total = 0;
    unsigned long t0, dt;

    if (tcp_create()) { LOG("bulk recv: TCPCreate failed\r"); return; }
    r = tcp_open(g_host, BULK_SRC, &connect_ticks);
    if (r) { LOG("bulk recv: TCPActiveOpen -> %d\r", (int)r); tcp_abort_release(); return; }
    t0 = ticks();
    for (;;) {
        uint16_t got = 0;
        r = tcp_rcv(&got, 20);
        if (r == 0) total += got;      /* on an error rcvBuffLen still holds the buffer size */
        if (r == connectionClosing) break;
        if (r != 0 && r != commandTimeout) break;
        if (r == commandTimeout && got == 0) break;
    }
    dt = ticks() - t0;
    LOG("bulk recv from %d: %lu bytes in %lu ticks (%lu B/s), ended by %d\r",
        BULK_SRC, (unsigned long)total, dt,
        dt ? (unsigned long)((uint32_t)((uint64_t)total * 60 / dt)) : 0, (int)r);
    tcp_close_release();
}

static void bench_bulk_send(void)
{
    static uint8_t chunk[1400];
    long connect_ticks = 0;
    int16_t r;
    uint32_t sent = 0;
    unsigned long t0, dt;
    int i;

    for (i = 0; i < (int)sizeof chunk; i++) chunk[i] = (uint8_t)i;
    if (tcp_create()) { LOG("bulk send: TCPCreate failed\r"); return; }
    r = tcp_open(g_host, BULK_SINK, &connect_ticks);
    if (r) { LOG("bulk send: TCPActiveOpen -> %d\r", (int)r); tcp_abort_release(); return; }
    t0 = ticks();
    while (sent < BULK_SEND_TOTAL) {
        uint16_t n = (uint16_t)((BULK_SEND_TOTAL - sent) < sizeof chunk
                                ? (BULK_SEND_TOTAL - sent) : sizeof chunk);
        if (tcp_send(chunk, n)) break;
        sent += n;
    }
    dt = ticks() - t0;
    LOG("bulk send to %d: %lu bytes in %lu ticks (%lu B/s)\r",
        BULK_SINK, (unsigned long)sent, dt,
        dt ? (unsigned long)((uint32_t)((uint64_t)sent * 60 / dt)) : 0);
    tcp_close_release();
}

static void read_cfg(void)
{
    short r = -1;
    long len = 0;
    char buf[64];
    int a, b, c, d;
    if (FSOpen((ConstStr255Param)"\pTcpBench.cfg", 0, &r) != noErr) return;
    GetEOF(r, &len);
    if (len > 63) len = 63;
    FSRead(r, &len, (Ptr)buf);
    buf[len] = 0;
    if (sscanf(buf, "%d.%d.%d.%d", &a, &b, &c, &d) == 4)
        g_host = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    g_yield = strstr(buf, "yield") != 0;
    FSClose(r);
}

int main(void)
{
    Str255 ps;
    size_t n;
    int16_t r;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
    runid_name("TcpBench", ".out", g_outname);

    FSDelete((ConstStr255Param)g_outname, 0);
    Create((ConstStr255Param)g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStr255Param)g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
    wr("TcpBench (MacTCP .IPP TCP benchmark)\r");

    read_cfg();
    if (id_says_both(g_outname, 8)) g_yield = 1;          /* "TcpBench-both..." */
    LOG("host %08lx%s\r", (unsigned long)g_host, g_yield ? " (yield: side-by-side mode)" : "");

    r = (int16_t)OpenDriver((ConstStr255Param)"\p.IPP", &g_ipp);
    if (r) { LOG("OpenDriver(.IPP) failed: %d (is MacTCP installed?)\r", (int)r); goto fin; }
    LOG("OpenDriver(.IPP) ok, refnum %d\r", (int)g_ipp);

    bench_connect_and_echo();
    (void)g32;
    bench_bulk_recv();
    bench_bulk_send();

    wr("RESULT DONE\r");
fin:
    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);
    sprintf(g_line, "TcpBench finished. Results are in %.*s", (int)g_outname[0], (const char *)g_outname + 1);
    n = strlen(g_line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, g_line, n);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

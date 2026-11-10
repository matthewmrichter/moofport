/* afpcopy.c - AfpCopy: copy a file to an AppleShare volume and back over AppleTalk, check every
 * byte, and record the MoofPort driver's counters around each copy.
 *
 * The file test is the PLAN Phase 2 AppleShare row: AppleTalk (ASP/ATP/DDP) runs over ".ENET0",
 * so a byte-exact round trip of a few hundred KB through the File Manager proves MoofPort's
 * receive and transmit paths under a real AppleTalk client, not only NBP lookups.
 *
 *  1. Find the server volume: the one named in AfpCopy.cfg, else the first mounted volume
 *     with a non-zero file-system id (ioVFSID; HFS and MFS volumes are 0, AppleShare is not).
 *     Waits up to `wait` seconds for it to appear. System 6 has no call to mount a server
 *     volume, so mount it first: by hand in the Chooser, or tick the volume's "open at system
 *     startup" box in the Chooser (with the name and password saved) so it mounts at boot and
 *     AfpCopy can run unattended.
 *  2. Write "AfpCopy src" next to the app: `kb` KB of data fork and `rsrc` KB of resource
 *     fork, both a reproducible pseudo-random pattern.
 *  3. Copy it to "AfpCopy test" in the volume's root (both forks), timed.
 *  4. Copy that back to "AfpCopy back" next to the app, timed.
 *  5. Compare "AfpCopy back" with the pattern, byte for byte.
 * MoofPort's private Status call (mp_drv.h) is read before and after each copy; another
 * driver is reported as such and the test still runs. The server copy is kept so the host can
 * check it: its data fork's CRC-32 (the zlib/`crc32` one) is in the output. The local files
 * are deleted at the end unless `keep`.
 *
 * AfpCopy.cfg (optional, next to the app), one setting per line:
 *     volume=MoofShare   kb=320   rsrc=16   wait=60   keep   yield
 * yield: give other applications time after each 16 KB (MultiFinder side-by-side runs, hw3 step
 * D). Off by default: yielding slows the copy (hw3), so measurements run without it. A run id
 * starting with "both" turns it on too.
 * Output: AfpCopy-<run id>.out next to the app (runid.c), last line "RESULT PASS" or
 * "RESULT FAIL ...".
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

#define BUF_SIZE  16384L
#define ROOT_DIR  2L              /* dirID of a volume's root directory */
#define SEED_DATA 0x4D6F6F66u     /* 'Moof' */
#define SEED_RSRC 0x506F7274u     /* 'Port' */

static short g_outref = -1;
static Str255 g_outname;
static char g_line[400];
static void wr(const char *s) { long n = (long)strlen(s); if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s); }
#define LOG(...) do { sprintf(g_line, __VA_ARGS__); wr(g_line); } while (0)

/* settings (AfpCopy.cfg) */
static unsigned char g_volname[32];   /* Pascal; empty = first server volume */
static long g_kb = 320, g_rsrc_kb = 16, g_wait = 60;
static int g_keep, g_yield;

/* A run id starting with "both" (e.g. "both", "both2") also turns yielding on: the side-by-side
 * step then needs no edited settings file. */
static int id_says_both(const unsigned char *outname, int toollen)
{
    return outname[0] >= toollen + 5 && !memcmp(outname + 1 + toollen + 1, "both", 4);
}

static uint8_t *g_buf;
static char g_fail[120];
static void fail(const char *what, int err)
{
    if (!g_fail[0]) sprintf(g_fail, "%s (%d)", what, err);
    LOG("FAIL: %s, error %d\r", what, err);
}

/* ---- the pattern: xorshift32 bytes, so any stretch can be regenerated to compare ---- */

static uint32_t pat_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static void pat_fill(uint32_t *s, uint8_t *p, long n)
{
    long i;
    for (i = 0; i + 4 <= n; i += 4) {
        uint32_t v = pat_next(s);
        p[i] = (uint8_t)(v >> 24); p[i + 1] = (uint8_t)(v >> 16);
        p[i + 2] = (uint8_t)(v >> 8); p[i + 3] = (uint8_t)v;
    }
    if (i < n) {
        uint32_t v = pat_next(s);
        for (; i < n; i++, v <<= 8) p[i] = (uint8_t)(v >> 24);
    }
}

static uint32_t g_crctab[256];
static void crc_init(void)
{
    uint32_t i, c;
    int k;
    for (i = 0; i < 256; i++) {
        for (c = i, k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crctab[i] = c;
    }
}
static uint32_t crc_add(uint32_t crc, const uint8_t *p, long n)
{
    crc = ~crc;
    while (n-- > 0) crc = g_crctab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- AfpCopy.cfg ---- */

static long num_after(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

static void read_cfg(void)
{
    short r;
    long len = 255, i = 0;
    static char buf[256];
    if (FSOpen((ConstStr255Param)"\pAfpCopy.cfg", 0, &r) != noErr) return;
    if (FSRead(r, &len, (Ptr)buf) != noErr && len <= 0) len = 0;
    FSClose(r);
    buf[len] = 0;
    while (i < len) {
        char *l = buf + i;
        long e = i;
        while (e < len && buf[e] != '\r' && buf[e] != '\n') e++;
        buf[e] = 0;
        if (!strncmp(l, "volume=", 7)) {
            size_t n = strlen(l + 7);
            if (n > 27) n = 27;
            g_volname[0] = (unsigned char)n;
            memcpy(g_volname + 1, l + 7, n);
        } else if (!strncmp(l, "kb=", 3)) {
            g_kb = num_after(l + 3);
        } else if (!strncmp(l, "rsrc=", 5)) {
            g_rsrc_kb = num_after(l + 5);
        } else if (!strncmp(l, "wait=", 5)) {
            g_wait = num_after(l + 5);
        } else if (!strncmp(l, "keep", 4)) {
            g_keep = 1;
        } else if (!strncmp(l, "yield", 5)) {
            g_yield = 1;
        }
        i = e + 1;
    }
    if (g_kb < 1) g_kb = 1;
    if (g_kb > 4096) g_kb = 4096;
    if (g_rsrc_kb < 0) g_rsrc_kb = 0;
    if (g_rsrc_kb > 512) g_rsrc_kb = 512;
}

/* ---- the server volume ---- */

static int same_name(const unsigned char *a, const unsigned char *b)
{
    int i;
    if (a[0] != b[0]) return 0;
    for (i = 1; i <= a[0]; i++) {
        unsigned char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (unsigned char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (unsigned char)(y - 32);
        if (x != y) return 0;
    }
    return 1;
}

static void log_pstr(const char *pre, const unsigned char *p, const char *post)
{
    LOG("%s%.*s%s", pre, (int)p[0], (const char *)p + 1, post);
}

/* List the mounted volumes; return the server volume's vRefNum, or 0. */
static short find_volume(int verbose)
{
    HParamBlockRec pb;
    Str255 name;
    short i, found = 0;
    for (i = 1; ; i++) {
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.volumeParam.ioNamePtr = name;
        pb.volumeParam.ioVolIndex = i;
        if (PBHGetVInfoSync(&pb) != noErr) break;
        if (verbose)
            LOG("volume %d: \"%.*s\" vRefNum %d fsid 0x%04x\r", (int)i, (int)name[0],
                (const char *)name + 1, (int)pb.volumeParam.ioVRefNum,
                (unsigned)(uint16_t)pb.volumeParam.ioVFSID);
        if (found) continue;
        if (g_volname[0] ? same_name(name, g_volname) : pb.volumeParam.ioVFSID != 0)
            found = pb.volumeParam.ioVRefNum;
    }
    return found;
}

/* ---- MoofPort's counters ---- */

static short g_enet;
static int g_is_moofport;

static int get_stats(MpStats *st)
{
    EPB pb;
    if (!g_is_moofport) return -1;
    memset(&pb, 0, sizeof pb);
    memset(st, 0, sizeof *st);
    pb.ioRefNum = g_enet;
    pb.csCode = MP_STATUS_STATS;
    pb.ePointer = (uint32_t)st;
    pb.eBuffSize = (int16_t)sizeof *st;
    return (PBStatusSync((ParmBlkPtr)&pb) == noErr && st->version == MP_STATS_VERSION) ? 0 : -1;
}

static void open_enet(void)
{
    MpStats st;
    if (OpenDriver((ConstStr255Param)"\p.ENET0", &g_enet) != noErr) {
        LOG(".ENET0 is not open (OpenDriver failed): no driver counters\r");
        return;
    }
    g_is_moofport = 1;
    if (get_stats(&st)) {
        g_is_moofport = 0;
        LOG(".ENET0 refnum %d is not MoofPort: no driver counters\r", (int)g_enet);
        return;
    }
    LOG(".ENET0 is MoofPort, refnum %d, MAC %02x:%02x:%02x:%02x:%02x:%02x\r", (int)g_enet,
        st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
}

#define D(f) (unsigned long)(b->f - a->f)
static void log_delta(const char *phase, const MpStats *a, const MpStats *b)
{
    if (!g_is_moofport) return;
    LOG("  driver %s: rx frames %lu, delivered %lu, reads %lu (empty %lu), overflow %lu, "
        "drained %lu, aborted %lu, bad %lu\r", phase, D(rx.frames), D(delivered), D(rx.polls),
        D(rx.empty), D(rx.overflow), D(rx.drained), D(rx.aborted), D(rx.bad));
    LOG("  driver %s: tx sent %lu, now %lu, deferred %lu, failed %lu, full %lu, errors %lu; "
        "busy %lu, bus errors %lu, unattached %lu\r", phase, D(tx.sent), D(tx_now),
        D(tx_deferred), D(tx_failed), D(tx.full), D(tx.errors), D(busy), D(bus_err),
        D(unattached));
}
#undef D

/* ---- files ---- */

/* Open a fork of (vref, dirID, name); vref 0 = the app's folder. mode OPEN_READ, OPEN_WRITE, or
 * OPEN_NEW (delete, create, write). Returns the refnum or 0. */
enum { OPEN_READ, OPEN_WRITE, OPEN_NEW };
static short open_fork(short vref, long dir, ConstStr255Param name, int rsrc, int mode)
{
    HParamBlockRec pb;
    OSErr r;
    if (mode == OPEN_NEW) {
        memset(&pb, 0, sizeof pb);
        pb.fileParam.ioNamePtr = (StringPtr)name;
        pb.fileParam.ioVRefNum = vref;
        pb.fileParam.ioDirID = dir;
        PBHDeleteSync(&pb);
        memset(&pb, 0, sizeof pb);
        pb.fileParam.ioNamePtr = (StringPtr)name;
        pb.fileParam.ioVRefNum = vref;
        pb.fileParam.ioDirID = dir;
        r = PBHCreateSync(&pb);
        if (r) { fail("create", r); return 0; }
    }
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    pb.ioParam.ioPermssn = mode == OPEN_READ ? fsRdPerm : fsRdWrPerm;
    r = rsrc ? PBHOpenRFSync(&pb) : PBHOpenSync(&pb);
    if (r) { fail(rsrc ? "open resource fork" : "open data fork", r); return 0; }
    return pb.ioParam.ioRefNum;
}

static void delete_file(short vref, long dir, ConstStr255Param name)
{
    HParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    pb.fileParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    PBHDeleteSync(&pb);
}

/* Write `len` bytes of pattern `seed` to an open fork; returns the CRC-32 or sets g_fail. */
static uint32_t write_pattern(short ref, uint32_t seed, long len)
{
    uint32_t s = seed, crc = 0;
    long done = 0;
    OSErr r;
    SetEOF(ref, 0);
    while (done < len) {
        long n = len - done < BUF_SIZE ? len - done : BUF_SIZE;
        pat_fill(&s, g_buf, n);
        crc = crc_add(crc, g_buf, n);
        if ((r = FSWrite(ref, &n, (Ptr)g_buf)) != noErr) { fail("write", r); break; }
        done += n;
    }
    return crc;
}

/* Let other applications run between chunks (MultiFinder; TcpBench runs beside AfpCopy in hw3). */
static void yield(void)
{
    EventRecord ev;
    if (g_yield)
        WaitNextEvent(everyEvent, &ev, 0, 0);
}

/* Copy one fork of `len` bytes, yielding between chunks; returns bytes copied. */
static long copy_fork(short from, short to, long len)
{
    long done = 0;
    OSErr r;
    SetEOF(to, 0);
    while (done < len) {
        long n = len - done < BUF_SIZE ? len - done : BUF_SIZE, m;
        r = FSRead(from, &n, (Ptr)g_buf);
        if (r != noErr && !(r == eofErr && n > 0)) { fail("read", r); break; }
        m = n;
        if ((r = FSWrite(to, &m, (Ptr)g_buf)) != noErr || m != n) { fail("write", r); break; }
        done += n;
        yield();
    }
    return done;
}

/* Compare an open fork with pattern `seed` of `len` bytes; returns the first bad offset or -1.
 * For a resource fork, bytes 16-127 (the header's "reserved for system use" area) are not
 * compared: System 6 writes the file's name there (seen at offset 47) when the fork is closed. */
static long compare_fork(short ref, uint32_t seed, long len, int rsrc, uint32_t *crc_out)
{
    static uint8_t want[BUF_SIZE];
    uint32_t s = seed, crc = 0;
    long done = 0, eof = 0, i;
    OSErr r;
    GetEOF(ref, &eof);
    if (eof != len) {
        LOG("  length %ld, expected %ld\r", eof, len);
        if (eof < len) len = eof;
    }
    while (done < len) {
        long n = len - done < BUF_SIZE ? len - done : BUF_SIZE, got = n;
        r = FSRead(ref, &got, (Ptr)g_buf);
        if ((r != noErr && r != eofErr) || got != n) { fail("read back", r); return done; }
        pat_fill(&s, want, n);
        crc = crc_add(crc, g_buf, n);
        for (i = 0; i < n; i++)
            if (g_buf[i] != want[i] && !(rsrc && done + i >= 16 && done + i < 128)) {
                LOG("  first difference at offset %ld: got 0x%02x, expected 0x%02x\r",
                    done + i, g_buf[i], want[i]);
                return done + i;
            }
        done += n;
    }
    *crc_out = crc;
    return eof == len ? -1 : len;
}

/* Copy both forks of (sv, sd, sn) to (dv, dd, dn); returns ticks taken, or -1. */
static long copy_file(short sv, long sd, ConstStr255Param sn, short dv, long dd, ConstStr255Param dn,
                      long dlen, long rlen)
{
    unsigned long t0 = TickCount();
    short a, b;
    long n;
    if (!(a = open_fork(sv, sd, sn, 0, OPEN_READ))) return -1;
    if (!(b = open_fork(dv, dd, dn, 0, OPEN_NEW))) { FSClose(a); return -1; }
    n = copy_fork(a, b, dlen);
    FSClose(a); FSClose(b);
    if (n != dlen) return -1;
    if (rlen) {
        if (!(a = open_fork(sv, sd, sn, 1, OPEN_READ))) return -1;
        if (!(b = open_fork(dv, dd, dn, 1, OPEN_WRITE))) { FSClose(a); return -1; }
        n = copy_fork(a, b, rlen);
        FSClose(a); FSClose(b);
        if (n != rlen) return -1;
    }
    FlushVol(0, dv);
    return (long)(TickCount() - t0);
}

static void log_rate(const char *what, long bytes, long ticks)
{
    long bps = ticks > 0 ? (long)((double)bytes * 60.0 / (double)ticks) : 0;
    LOG("%s: %ld bytes in %ld ticks (%ld.%02ld s), %ld bytes/s\r", what, bytes, ticks,
        ticks / 60, (ticks % 60) * 100 / 60, bps);
}

static const unsigned char kSrc[] = "\pAfpCopy src";
static const unsigned char kBack[] = "\pAfpCopy back";
static const unsigned char kTest[] = "\pAfpCopy test";

static void run(void)
{
    long dlen = g_kb * 1024L, rlen = g_rsrc_kb * 1024L, t, off;
    uint32_t dcrc, c;
    short vol = 0, ref;
    MpStats s0, s1, s2;
    unsigned long until;

    LOG("settings: volume \"%.*s\" kb %ld rsrc %ld wait %ld%s%s\r", (int)g_volname[0],
        (const char *)g_volname + 1, g_kb, g_rsrc_kb, g_wait, g_keep ? " keep" : "",
        g_yield ? " yield" : "");
    open_enet();

    until = TickCount() + (unsigned long)g_wait * 60;
    while (!(vol = find_volume(0)) && TickCount() < until) {
        EventRecord ev;
        WaitNextEvent(everyEvent, &ev, 60, 0);
    }
    find_volume(1);
    if (!vol) { fail("no AppleShare volume mounted (mount it in the Chooser)", -1); return; }
    LOG("server volume: vRefNum %d\r", (int)vol);

    if (!(g_buf = (uint8_t *)NewPtr(BUF_SIZE))) { fail("NewPtr", MemError()); return; }

    /* 1. the local source */
    if (!(ref = open_fork(0, 0, kSrc, 0, OPEN_NEW))) return;
    dcrc = write_pattern(ref, SEED_DATA, dlen);
    FSClose(ref);
    if (rlen) {
        if (!(ref = open_fork(0, 0, kSrc, 1, OPEN_WRITE))) return;
        write_pattern(ref, SEED_RSRC, rlen);
        FSClose(ref);
    }
    FlushVol(0, 0);
    if (g_fail[0]) return;
    LOG("source: data fork %ld bytes CRC-32 %08lx, resource fork %ld bytes\r", dlen,
        (unsigned long)dcrc, rlen);

    /* 2. to the server */
    get_stats(&s0);
    t = copy_file(0, 0, kSrc, vol, ROOT_DIR, kTest, dlen, rlen);
    get_stats(&s1);
    if (t < 0) return;
    log_rate("to server  ", dlen + rlen, t);
    log_delta("to server  ", &s0, &s1);

    /* 3. and back */
    t = copy_file(vol, ROOT_DIR, kTest, 0, 0, kBack, dlen, rlen);
    get_stats(&s2);
    if (t < 0) return;
    log_rate("from server", dlen + rlen, t);
    log_delta("from server", &s1, &s2);

    /* 4. byte for byte */
    if (!(ref = open_fork(0, 0, kBack, 0, OPEN_READ))) return;
    off = compare_fork(ref, SEED_DATA, dlen, 0, &c);
    FSClose(ref);
    if (off >= 0) { fail("data fork differs after the round trip", (int)(off & 0x7FFF)); return; }
    LOG("data fork round trip: %ld bytes identical, CRC-32 %08lx\r", dlen, (unsigned long)c);
    if (rlen) {
        if (!(ref = open_fork(0, 0, kBack, 1, OPEN_READ))) return;
        off = compare_fork(ref, SEED_RSRC, rlen, 1, &c);
        FSClose(ref);
        if (off >= 0) { fail("resource fork differs after the round trip", (int)(off & 0x7FFF)); return; }
        LOG("resource fork round trip: %ld bytes identical (bytes 16-127 are the system's)\r", rlen);
    }
    log_pstr("the server copy stays as \"", kTest, "\" in the volume's root\r");
}

int main(void)
{
    Str255 ps;
    size_t n;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
    crc_init();
    runid_name("AfpCopy", ".out", g_outname);
    FSDelete(g_outname, 0);
    Create(g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen(g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
    wr("AfpCopy\r");

    read_cfg();
    if (id_says_both(g_outname, 7)) g_yield = 1;          /* "AfpCopy-both..." */
    run();
    if (!g_keep) {
        delete_file(0, 0, kSrc);
        delete_file(0, 0, kBack);
    }
    if (g_fail[0]) LOG("RESULT FAIL %s\r", g_fail);
    else wr("RESULT PASS\r");

    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);
    sprintf(g_line, "AfpCopy: %s. See %.*s", g_fail[0] ? "FAIL" : "PASS", (int)g_outname[0],
            (const char *)g_outname + 1);
    n = strlen(g_line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, g_line, n);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

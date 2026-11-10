/* mp_drv.c - the MoofPort .ENET0 driver, Phase 1 (PLAN section 4): a minimum working driver.
 *
 * The Device Manager entries come from mp_drv_stubs.s; the frame engine is the portable core
 * (core/: mp_rx, mp_tx, mp_demux, mp_ring) on the SCSI Manager glue (mp_scsi.c).
 *
 *   Open      first open: find the DaynaPORT (INQUIRY), enable it, read its MAC, start the poll
 *             task. Nothing touches the device before this, so an open at INIT time (AppleTalk
 *             with EtherTalk) is as safe as one from MacTCP: nothing here reads CurStackBase,
 *             CurrentA5 or other application globals (docs/dev/BOOT-CRASH-ETHERTALK.md).
 *   Control   the Ethernet csCodes MacTCP uses (docs/dev/INTERFACE.md): EGetInfo, ESetGeneral
 *             (accepted, result 0), EAttachPH, EDetachPH, EWrite; EAddMulti/EDelMulti accepted.
 *   Poll      a Time Manager task. Each tick does one kind of heavy work: READs (each
 *             delivered once its SCSI transaction ended) until rx_budget frames came (2, or 4
 *             with MoofTune rx4), a READ came back empty or a TX frame is waiting; or, if no
 *             frame came, one WRITE (one frame; MoofTune batch: all queued frames, new firmware).
 *             After a WRITE the next tick comes ACK_MS later (no READ sooner: hwsend5 lost
 *             frames with READs 2 ms after a WRITE), then the reply chase. A READ that brings
 *             only small frames (a reply) ends the tick at once, so MacTCP's own send task runs. After a tick that brought frames, a
 *             waiting TX frame goes first. Empty READs are cheap. When work is left the next
 *             tick comes at once. A tick stays well under one VBL period, so the
 *             60 Hz tick (Ticks, the System's timers) is not lost (hwsend: 65% lost when ticks
 *             ran 20-37 ms with 13 ms WRITEs inside MacTCP's interrupt-level EWrites).
 *             The interval comes from the core's mp_poll. 1.0 defaults (decision 6): 5 ms for
 *             60 ms after traffic (hw6: receive +9%), one tick until 2 s of quiet, then out to
 *             three ticks (hw6: 36% fewer empty polls on an idle Mac). Every empty READ costs
 *             about 3.3 ms of CPU on the Classic (hw4), which is why the burst is short and the
 *             idle rate low; a 3 ms burst cut TCP send to a third (hw3).
 *   EWrite    copies the frame into the TX ring and completes at once; the poll task sends it
 *             about a millisecond later (kick), and polls at burst_ms after it for the reply.
 *             MacTCP calls EWrite at interrupt level, so a WRITE done there (5-13 ms) held off
 *             every other interrupt. MoofTune inline (hwsend7, off by default): with nothing
 *             queued, an interrupt-level EWrite does that one WRITE itself, as DaynaPORT does. When both TX slots are full an async call stays pending
 *             (all of MacTCP's EWrites are async, docs/dev/INTERFACE.md section 4): the Device
 *             Manager holds the calls behind it, so order is kept, and the poll task copies the
 *             frame and completes the call with IODone once a slot is free. A sync call made at
 *             task level is left pending the same way; an immediate call, or a sync one at
 *             interrupt level, gets one WRITE of the ring first and is refused (counted in
 *             tx_failed) if the bus is busy. Never hangs.
 *   Tune      private Control MP_CTL_TUNE (mp_drv.h): poll timing, blind payload reads and frame
 *             writes, batched WRITEs, frames per tick, the reply chase, and (diagnostic build
 *             only since send-fix, to fit the memory cap) a verify mode that checks the
 *             IPv4/TCP/UDP checksums of every received frame (the hardware gate for blind reads,
 *             PLAN 2.2, passed in hw3). Blind payload reads are ON by
 *             default since the gate passed (hw3: 697 full frames, 0 bad; decision 5), blind
 *             frame writes since hwsend (safe in hw5; a full-frame WRITE took 13.4 ms polled); the
 *             header, the preamble, the trailer and the device's pauses stay handshaked.
 *
 * EWrite is a Control call, so the Device Manager queue serialises it: there is one TX producer
 * at a time (the poll task copies a pending call's frame only while that call holds the queue),
 * and the poll task is the only consumer. The poll task is also the only RX producer and
 * consumer.
 *
 * All state is one block in the system heap (MpDrv), allocated by the INIT at boot through
 * mp_drv_boot. The code is position-independent (-mpcrel): it needs no relocation, so Retro68's
 * relocator is not linked in, and its one global lives in .data (no A5 world).
 */
#include <Devices.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Timer.h>
#include <string.h>
#include "mp.h"
#include "mp_scsi.h"
#include "mp_drv.h"
#include "mp_ipck.h"

/* The hwsend diagnostic build (-DMP_DIAG, target MoofPortDiagDrv) adds timing hooks (mp_diag.c);
 * without it DIAG(...) is empty and the driver is the shipping one, byte for byte. */
#ifdef MP_DIAG
#define MP_DIAG_DRIVER
#include "mp_diag.h"
#define DIAG(...) __VA_ARGS__
#else
#define DIAG(...)
#endif

#pragma GCC diagnostic ignored "-Warray-bounds"

#define RX_SLOTS   3              /* PLAN 2.4 default: 3 RX + 2 TX */
#define TX_SLOTS   2
/* 1.0 defaults (decision 6, after hw6): 5 ms for 60 ms after traffic, one tick until 2 s of
 * quiet, then one more tick per poll up to three. MP_CTL_TUNE changes them (MoofTune presets). */
#define BURST_MS   5
#define HOLD_MS    60
#define MID_MS     17             /* one tick, DaynaPORT's rate */
#define QUIET_MS   2000
#define IDLE_MS    51             /* three ticks */
#define STEP_MS    17
#define BUSY_MS    2              /* retry delay when the SCSI Manager is busy */
#define TX_MS      1              /* an EWrite brings the next tick this close */
#define ACK_MS     5              /* the first poll after a WRITE, and no READ sooner (hwsend5: READs
                                     2 ms after a WRITE went with lost frames; MoofTune ack2) */
#define HUSH_MS    15             /* MoofTune hush (hwsend8): no poll this long after any reply */
#define PAUSE_MS   12             /* the reply pause: under MacTCP's ~13.5 ms to its next send */
#define CHASE_N    3              /* then up to this many more polls ack_ms apart until it comes */
#define SMALL_RX   128            /* a frame this short is a reply (an ACK): hand it over and yield */
#define MAX_READS  4              /* READs per poll */
#define RX_BUDGET  2              /* frames per poll before the tick gives way (VBL, TX); hwsend6:
                                     2 lost half the ticks 4 did in receive, at the same speed */
#define MAX_WDS    16             /* EWrite pieces */

/* ENET.h: ENetDelMulti 245, ENetAddMulti 246 (AppleTalk calls 246 with 09:00:07:FF:FF:FF). */
enum { E_DELMULTI = 245, E_ADDMULTI = 246, E_ATTACH = 247, E_DETACH = 248, E_WRITE = 249,
       E_READ = 250, E_RDCANCEL = 251, E_GETINFO = 252, E_SETGENERAL = 253 };
enum { eLenErr = -92 };          /* controlErr, statusErr, openErr, abortErr: Multiverse.h */
#define PENDING    1              /* control(): leave the call pending (mp_drv_stubs.s) */

/* The Ethernet parameter block (csParam at 28): docs/dev/INTERFACE.md. */
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
} EPB;
#pragma pack(pop)

/* Two poll timers, only ever primed while idle (never RmvTime/InsTime'd while running): the poll
 * runs on whichever was primed last (gen); to bring the next poll forward, kick() primes the idle
 * one, and the other, when it fires, finds itself stale and does nothing. hwsend9: re-arming a
 * primed timer with RmvTime/InsTime/PrimeTime from inside MacTCP's Time Manager task, right after
 * a 7 ms inline WRITE, left polling dead for about 1.1 s at a time. */
typedef struct MpTimer {
    TMTask tm;                    /* first: the Time Manager hands us its address */
    volatile uint8_t armed;       /* primed and not yet fired */
    uint8_t gen;                  /* d->gen when it was primed */
} MpTimer;

typedef struct MpDrv {
    MpTimer t[2];
    uint8_t gen;                  /* bumped at each prime: the newest timer carries the poll */
    uint8_t stop;                 /* closing: no more primes */
    MpScsi scsi;
    mp_bus bus;
    mp_ring rxr, txr;
    mp_rx rx;
    mp_tx tx;
    mp_demux dm;
    uint8_t mac[6];
    uint8_t opened;
    uint8_t rx_last;              /* the last tick's READ brought frames */
    uint32_t polls, busy, delivered, tx_now, tx_deferred, tx_failed, bus_err;
    int16_t last_err;
    mp_poll poll;
    uint16_t sched_ms;            /* the interval the timer was last primed with */
    uint8_t in_tick;              /* 1 while mp_drv_tick runs (nested EWrites do not kick) */
    uint8_t tx_in_tick;           /* a handler sent during this tick */
    uint8_t verify;
    uint8_t rx_budget;            /* frames per tick (RX_BUDGET, or 4 with MP_TUNE_RX4) */
    uint8_t ack_ms;               /* ACK_MS, or 2 with MP_TUNE_ACK2 */
    uint8_t chase;                /* polls left chasing the reply to a WRITE; 0xFF = chase off */
    uint8_t wrote_last;           /* the last tick WROTE (no READ until ack_ms after it) */
    uint8_t rx_big;               /* this tick delivered a frame longer than SMALL_RX */
    uint8_t yield;                /* end the tick after delivering only small frames (MoofTune noyield) */
    uint8_t inline_tx;            /* EWrite writes at once when the TX ring is empty (MoofTune noinline) */
    uint8_t inl;                  /* an inline WRITE since the last tick started (no second one) */
    uint8_t hush;                 /* MoofTune hush: its MP_TUNE_HUSH* flag bits (0 = off) */
    uint8_t rx_other;             /* this tick delivered a frame that isn't a small TCP one */
    uint8_t early;                /* MP_EARLY_*: how an inline WRITE's reply is chased (MoofTune chase*) */
    uint8_t cms;                  /* the chase's spacing: ack_ms after a tick's WRITE, 1-2 after inline */
    uint8_t flags2;               /* MpTune.flags2: MP_TUNE2_NOPAUSE, MP_TUNE2_REPOLL */
    uint8_t tflags;               /* MpTune.flags as last set (0 = the defaults), for Status */
    uint32_t kicks, v_ok, v_bad, v_big;
    struct EPB *parked;           /* an EWrite waiting for a free TX slot */
    void *dce;                    /* ... its DCE, for IODone */
    MpCalls calls;                /* what clients ask (MP_STATUS_LOG) */
    VBLTask wdv;                  /* the watchdog (mp_drv_wd), every 100 ms */
    uint32_t wd, wd_polls;        /* times it re-armed the poll task; polls at its last look */
    uint16_t wd_ms;               /* ms without a poll so far */
    DIAG(uint8_t test_spin;)      /* MpTune.pad, diagnostic build: ms to spin after an inline WRITE */
    DIAG(MpDiag dg;)
    uint8_t rxmem[RX_SLOTS * MP_SLOT_SIZE];
    uint8_t txmem[TX_SLOTS * MP_SLOT_SIZE];
} MpDrv;


/* Set once by mp_drv_boot. In .data, not .bss: the driver is built position-independent
 * (-mpcrel) and never relocated, and a flat image has no room for a .bss after it. */
static MpDrv *g_d __attribute__((section(".data")));

#ifdef MP_DIAG
static int diag_flush(MpDrv *d);
#define TX_FLUSH(d) diag_flush(d)
#else
#define TX_FLUSH(d) mp_tx_flush(&(d)->tx, &(d)->bus)
#endif

long mp_drv_boot(void *drvr);                                        /* below */
extern void mp_tm_entry(void);                                     /* mp_drv_ph.s */
extern void mp_wd_entry(void);                                     /* mp_drv_ph.s */
extern void mp_call_ph(void *handler, uint8_t *frame, unsigned long len); /* mp_drv_ph.s */
extern void mp_iodone(void *dce, long result);                       /* mp_drv_stubs.s */
static int16_t equeue(MpDrv *d, struct EPB *pb);

/* ---- the poll task (Time Manager, interrupt level) ---- */

/* Clock for mp_poll: ms from Ticks (x17, 2% fast; the hold window is tick-grained anyway). */
static uint32_t now_ms(void) { return *(volatile uint32_t *)0x16A * 17u; }

/* Verify mode (MP_CTL_TUNE): check the checksums of every received IPv4 frame. */
#ifdef MP_DIAG
static void verify_frame(MpDrv *d, const uint8_t *f, uint16_t len)
{
    switch (mp_ip_check(f, len)) {
    case MP_IPCK_BAD:
        d->v_bad++;
        break;
    case MP_IPCK_FULL:
        d->v_ok++;
        if (len >= 1000)
            d->v_big++;
        break;
    case MP_IPCK_HDR:
        d->v_ok++;
        break;
    default:
        break;
    }
}
#endif

static void deliver(MpDrv *d)
{
    const uint8_t *f;
    uint16_t len;
    while ((f = mp_ring_peek(&d->rxr, &len)) != NULL) {
        mp_ph *e;
        DIAG(if (d->verify)
            verify_frame(d, f, len);)
        if (len > SMALL_RX)
            d->rx_big = 1;
        if (len > SMALL_RX || mp_rd16(f + 12) != 0x0800 || f[23] != 6)
            d->rx_other = 1;        /* not an IPv4 TCP reply: AppleTalk, ARP, UDP, data */
        e = mp_demux_lookup(&d->dm, f, len);
        if (e && e->handler) {
            DIAG(mp_diag_deliver_in(&d->dg);)
            mp_call_ph(e->handler, (uint8_t *)f, (unsigned long)len);
            DIAG(mp_diag_deliver_out(&d->dg);)
            d->delivered++;
        }
        mp_ring_pop(&d->rxr);
    }
}

/* The ring filled in the middle of a READ (Snow sends up to 8 packets per READ whatever we ask):
 * deliver what is there now, with the bus still held, instead of dropping the rest. A handler's
 * EWrite meanwhile finds the SCSI Manager busy and is deferred to the TX ring. */
static void full_cb(void *ctx)
{
    deliver((MpDrv *)ctx);
}

/* Called by mp_tm_entry with the TMTask. Re-arms itself unless stopped. */
static void prime(MpDrv *d, MpTimer *t, uint16_t ms)
{
    t->armed = 1;
    t->gen = ++d->gen;
    d->sched_ms = ms;
    PrimeTime((QElemPtr)&t->tm, ms);
}

void mp_drv_tick(MpTimer *t);
void mp_drv_tick(MpTimer *t)
{
    MpDrv *d = g_d;
    uint16_t next;
    uint8_t ev = 0;
    int i, more = 0, rc, rx, cut, wrote, ycut = 0, chasing;
    t->armed = 0;
    if (!d || d->stop || t->gen != d->gen)
        return;                     /* a stale timer: a kick primed the other one since */
    d->in_tick = 1;
    DIAG(mp_diag_tick_in(&d->dg);)
    d->tx_in_tick = 0;
    d->polls++;
    rx = cut = 0;
    d->rx_big = 0;
    d->rx_other = 0;
    d->inl = 0;
    /* One kind of heavy work per tick (top of file): READs while frames come, up to RX_BUDGET
     * and only while no TX frame waits, unless a TX frame waited through the last tick's
     * frames; then a WRITE if no frames came. */
    for (i = 0; i < MAX_READS && !(d->rx_last && mp_ring_used(&d->txr)) &&
                !(d->wrote_last && d->sched_ms < d->ack_ms); i++) {
        uint16_t free = mp_ring_free(&d->rxr);
        if (free == 0)
            break;
        if (d->flags2 & MP_TUNE2_STEADY)
            free = 1;               /* one frame per READ, as DaynaPORT asks */
        rc = mp_rx_poll(&d->rx, &d->bus, MP_READ_MULTI, mp_dev_read_alloc(free, 1), 0, &more);
        if (rc == MP_E_BUSY) {
            d->busy++;
            ev |= MP_PE_BUSY;
            break;
        }
        if (rc < 0) {
            d->bus_err++;
            d->last_err = (int16_t)rc;
        }
        deliver(d);
        if (rc > 0) {
            ev |= MP_PE_RX;
            rx += rc;
            /* Only small frames (a reply): return now. MacTCP primes its own Time Manager task
             * to send its next segment when its handler sees the ACK, and that task can't run
             * until this one returns (docs/dev/MACTCP-SEND-TRIGGER.md); one more READ here cost
             * it 3.3 ms per segment (hwsend5). */
            if (rx >= d->rx_budget || mp_ring_used(&d->txr) || (d->flags2 & MP_TUNE2_STEADY)) {
                cut = 1;
                break;
            }
            if (d->yield && !d->rx_big) {
                cut = ycut = 1;     /* fewer small frames than the READ could carry: none left */
                break;
            }
            continue;
        }
        if (!more)
            break;
    }
    /* More may be queued: come back at once. Not after a yield cut (hwsend11, poll-path P3): the
     * READ asked for up to 3 frames and brought fewer small ones, so the firmware had no more,
     * and a READ 1 ms later only took 3 ms from MacTCP's next send. */
    if (more || (cut && (!ycut || (d->flags2 & MP_TUNE2_REPOLL))))
        ev |= MP_PE_MORE;
    d->rx_last = rx != 0;
    wrote = 0;
    if (!rx && mp_ring_used(&d->txr)) {
        rc = TX_FLUSH(d);
        wrote = rc > 0;
        if (rc == MP_E_BUSY) {
            ev |= MP_PE_BUSY;
        } else if (rc < 0) {        /* the device refused it: drop, do not spin */
            mp_ring_pop(&d->txr);
            d->tx_failed++;
        } else if (rc > 0) {
            ev |= MP_PE_TX;
        }
    }
    if (d->parked)
        (void)equeue(d, d->parked); /* completes it with IODone once a slot is free */
    d->wrote_last = (uint8_t)wrote;
    if (mp_ring_used(&d->txr))
        ev |= MP_PE_MORE;
    if (d->tx_in_tick)
        ev |= MP_PE_TX;
    next = mp_poll_after(&d->poll, now_ms(), ev);
    /* The reply chase: MacTCP sends its next segment once the ACK reaches its handler (about
     * 15 ms of its own work later; the wire shows no 60 Hz pacing, hwsend5 notes), so after a
     * WRITE poll every ack_ms until a frame comes, at most CHASE_N more times. */
    if (wrote)
        d->cms = d->ack_ms;
    chasing = d->chase != 0xFF && d->chase;
    if (d->chase != 0xFF)
        d->chase = wrote ? CHASE_N : (rx || !d->chase) ? 0 : (uint8_t)(d->chase - 1);
    if (next == 0)
        next = 1;                   /* more queued: as soon as the Time Manager allows */
    else if ((wrote || (d->chase != 0xFF && d->chase)) && next > d->cms)
        next = d->cms;
    /* MoofTune hush: the tick handed MacTCP only small TCP frames (its ACKs) and nothing waits
     * to be sent. MacTCP now works about 13.5 ms before it sends, and every READ meanwhile
     * (2.2 empty ones, about 3 ms each at interrupt level) comes out of its time (hwsend7,
     * ANALYSIS7 section 2). So no poll until its EWrite kicks the timer, or hush_ms pass. */
    if (d->hush && cut && !d->rx_other && !mp_ring_used(&d->txr) && !d->parked)
        next = HUSH_MS;
    /* The reply pause (poll-path P1): this tick handed MacTCP the reply to our own WRITE (a
     * chase was on) and only small TCP frames. MacTCP takes about 13.5 ms to its next send and
     * kicks the timer when it does; every READ meanwhile is ~3 ms out of its time. At most
     * PAUSE_MS: hush's longer silences (15-20 ms, after any reply) lost more frames. */
    else if (!(d->flags2 & MP_TUNE2_NOPAUSE) && chasing && ycut && !d->rx_other &&
             !mp_ring_used(&d->txr) && !d->parked)
        next = PAUSE_MS;
    DIAG(mp_diag_tick_out(&d->dg);)
    d->in_tick = 0;
    if (!d->stop)
        prime(d, t, next);
}

/* The watchdog, a VBL task every 100 ms: if no poll ran for 100 ms past the interval the timer
 * was primed with, the poll task is stuck (hwsend9: about 1.1 s pauses after the timer was re-armed
 * from inside MacTCP's Time Manager task); re-arm it here, outside any Time Manager task. */
void mp_drv_wd(VBLTask *v);
void mp_drv_wd(VBLTask *v)
{
    MpDrv *d = g_d;
    MpTimer *t;
    v->vblCount = 6;
    if (!d || d->stop || d->in_tick)
        return;
    if (d->polls != d->wd_polls) {
        d->wd_polls = d->polls;
        d->wd_ms = 0;
        return;
    }
    d->wd_ms = (uint16_t)(d->wd_ms + 100);
    if (d->wd_ms <= d->sched_ms + 100u)
        return;
    d->wd++;
    d->wd_ms = 0;
    t = !d->t[0].armed ? &d->t[0] : !d->t[1].armed ? &d->t[1] : 0;
    if (!t) {                       /* both primed and neither fired: start one over */
        t = &d->t[0];
        RmvTime((QElemPtr)&t->tm);
        InsTime((QElemPtr)&t->tm);
    }
    prime(d, t, 1);
}

/* An EWrite outside the poll task: switch to burst (for the reply) and bring the next tick to
 * TX_MS, which sends the frame. Interrupts are masked so the poll task cannot run in between;
 * the Time Manager calls are safe at any level. */
static void kick(MpDrv *d, uint16_t ms)
{
    uint16_t sr;
    if (d->in_tick) {
        d->tx_in_tick = 1;          /* mp_drv_tick reports it to mp_poll */
        return;
    }
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    (void)mp_poll_kick(&d->poll, now_ms());
    if (!d->stop && d->sched_ms > ms) {
        MpTimer *t = !d->t[0].armed ? &d->t[0] : !d->t[1].armed ? &d->t[1] : 0;
        if (t) {                    /* else both are primed: the poll comes at its time */
            prime(d, t, ms);
            d->kicks++;
        }
    }
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
}

static void tune_defaults(mp_poll_cfg *c)
{
    mp_poll_defaults(c);
    c->burst_ms = BURST_MS;
    c->hold_ms = HOLD_MS;
    c->idle_ms = IDLE_MS;
    c->step_ms = STEP_MS;
    c->mid_ms = MID_MS;
    c->quiet_ms = QUIET_MS;
    c->busy_ms = BUSY_MS;
}

/* ---- Control ---- */

/* Count every Control call by csCode. */
static void log_call(MpDrv *d, int16_t cs)
{
    d->calls.calls[(cs >= E_DELMULTI && cs <= E_SETGENERAL) ? cs - E_DELMULTI : 9]++;
}

/* EAddMulti: pass the address to the device (0x0D, 6 bytes of DATA OUT). The address is the
 * PB's eMultiAddr, six bytes inline at offset 28 (where eProtType and ePointer are for the other
 * calls), not a pointer. */
static int16_t addmulti(MpDrv *d, const EPB *pb)
{
    uint8_t cdb[6];
    const uint8_t *addr = (const uint8_t *)pb + 28;
    int rc;
    mp_cdb_mcast(cdb);
    rc = d->bus.begin(d->bus.ctx, cdb);
    if (rc == MP_E_BUSY)
        return noErr;               /* the firmware ignores the list anyway [fw]; do not fail it */
    if (rc)
        return controlErr;
    rc = d->bus.xout(d->bus.ctx, addr, 6, MP_XF_POLLED);
    if (d->bus.end(d->bus.ctx) != 0 || rc)
        return controlErr;
    return noErr;
}

/* Copy an EWrite's frame into the TX ring. PENDING if the ring is full (the call stays parked),
 * else the call's result. A parked call is completed here with IODone (from the poll task). */
static uint16_t wds(const EPB *pb, mp_seg *seg)
{
    const uint8_t *w = (const uint8_t *)pb->ePointer;
    uint16_t n = 0;
    while (n < MAX_WDS) {
        uint16_t len = mp_rd16(w);
        if (len == 0)
            break;
        seg[n].len = len;
        seg[n].p = (uint8_t *)mp_rd32(w + 2);
        n++;
        w += 6;
    }
    return n;
}

static int16_t equeue(MpDrv *d, EPB *pb)
{
    mp_seg seg[MAX_WDS];
    int16_t r = noErr;
    int rc;
    rc = mp_tx_queue(&d->tx, seg, wds(pb, seg));
    if (rc == MP_E_FULL)
        return PENDING;
    if (rc == MP_OK) {
        d->tx_deferred++;
        kick(d, TX_MS);
    } else {
        d->tx_failed++;
        r = rc == MP_E_LEN ? eLenErr : controlErr;
    }
    if (pb == d->parked) {
        d->parked = 0;
        mp_iodone(d->dce, r);
    }
    return r;
}

/* Both TX slots full: an async call, or a sync one made at task level (the Device Manager waits
 * for it while the poll task runs), stays pending; an immediate call, or a sync one made at
 * interrupt level (its wait would block the poll task), gets one WRITE of the ring first, and is
 * refused if that frees no slot (bus busy). hwsend3: refusing sync calls uncounted cost
 * AppleShare uploads their retry timers. */
#ifdef MP_DIAG
/* The frame log for an inline WRITE: MacTCP's first piece is only the 14-byte Ethernet header
 * (hwsend7), so gather the headers from the pieces first. */
static void diag_wlog_segs(MpDrv *d, const mp_seg *seg, uint16_t n, int rc)
{
    uint8_t h[64];
    uint16_t i, k = 0, c;
    for (i = 0; i < n && k < sizeof h; i++) {
        c = seg[i].len < sizeof h - k ? seg[i].len : (uint16_t)(sizeof h - k);
        memcpy(h + k, seg[i].p, c);
        k = (uint16_t)(k + c);
    }
    mp_diag_wlog(&d->dg, h, k, rc, 0, 1, d->scsi.blind_tx);
}
#endif

static int16_t ewrite(MpDrv *d, EPB *pb, void *dce)
{
    uint16_t sr;
    int16_t r;
    __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
    /* MoofTune inline: with nothing queued, write this frame at once, in MacTCP's call, as
     * DaynaPORT does (docs/dev/DAYNAPORT-REPLY-PATH.md), instead of in the next tick, and
     * without copying it (the core's zero-copy single-frame WRITE): one WRITE per call. Not
     * again until a poll tick has started (inl), nor right after a tick's own WRITE
     * (wrote_last): MacTCP sends a burst of segments in one run of its task, and those go to the
     * queue and the tick's batched WRITE, so WRITEs don't pile up in one interrupt. Not while
     * frames are coming in (rx_big: the last tick brought large ones), so MacTCP's ACKs in a
     * receive go with the tick, not as a WRITE each inside its call (hwsend7: receive lost
     * ticks 17% -> 25%). It doesn't hold off READs as a tick's WRITE does (wrote_last): an
     * inline WRITE just before a tick made that tick skip its READ and delayed the ACK
     * (hwsend7 mode C -18%). Only at interrupt level (MacTCP's calls), where our poll task
     * can't start in the middle of it. A busy bus, a short first piece or a failed WRITE: the
     * frame is queued for the tick, as usual. */
    if (d->inline_tx && (sr & 0x0700) && !d->in_tick && !d->wrote_last && !d->inl &&
        !d->rx_big && !mp_ring_used(&d->txr)) {
        mp_seg seg[MAX_WDS];
        int rc;
        /* Chase the ACK: a poll due during the WRITE runs as soon as MacTCP's task returns,
         * then every 1-2 ms (cms) for about 8 ms. Primed before the WRITE, while no timer has
         * fired in the meantime. */
        if (d->early != MP_EARLY_OFF)
            kick(d, 1);
        rc = mp_tx_send(&d->tx, &d->bus, seg, wds(pb, seg));
#ifdef MP_DIAG
        if (rc != MP_E_BUSY && rc != MP_E_LEN)
            diag_wlog_segs(d, seg, wds(pb, seg), rc == MP_OK ? 1 : rc);
#endif
#ifdef MP_DIAG
        if (d->test_spin) {         /* Snow only: stretch the WRITE to the hardware's ~7 ms */
            uint16_t n = (uint16_t)(d->test_spin * 780u);
            __asm__ volatile("1: dbf %0,1b" : "+d"(n));
        }
#endif
        if (rc == MP_OK) {
            d->tx_now++;
            d->inl = 1;             /* the next one waits for a tick (and goes in its batch) */
            /* The ACK is at the device 1-2 ms after the frame leaves, and a READ soon after a
             * WRITE loses nothing (hwsend8: frames went missing around long gaps instead). */
            d->cms = d->early == MP_EARLY_1 ? 1 : d->early == MP_EARLY_OFF ? d->ack_ms : 2;
            if (d->chase != 0xFF)
                d->chase = d->early == MP_EARLY_OFF ? CHASE_N : d->early == MP_EARLY_1 ? 8 : 4;
            if (d->early == MP_EARLY_OFF && !(d->flags2 & MP_TUNE2_STEADY))
                kick(d, d->ack_ms); /* the first poll when the ACK can be back (two-timer prime) */
            return noErr;
        }
    }
    r = equeue(d, pb);
    if (r != PENDING)
        return r;
    if (!(pb->ioTrap & 0x0200) && ((pb->ioTrap & 0x0400) || !(sr & 0x0700))) {
        d->parked = pb;
        d->dce = dce;
        return PENDING;
    }
    (void)TX_FLUSH(d);
    r = equeue(d, pb);
    if (r != PENDING)
        return r;
    d->tx_failed++;
    return controlErr;
}

#ifdef MP_DIAG
/* ewrite() between the diag stamps: nested (from a handler in the poll task) or not, sent at
 * once or deferred. */
static int16_t diag_ewrite(MpDrv *d, EPB *pb, void *dce)
{
    uint32_t failed0 = d->tx_failed;
    uint16_t sr;
    int16_t r;
    __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
    d->dg.st.ew_kind[((pb->ioTrap & 0x0200) ? 4 : (pb->ioTrap & 0x0400) ? 0 : 2) +
                     ((sr & 0x0700) ? 1 : 0)]++;
    mp_diag_ew_in(&d->dg, d->in_tick);
    r = ewrite(d, pb, dce);
    if (d->tx_failed != failed0 && r == controlErr)
        d->dg.st.ew_refused++;
    if (r == PENDING && !(pb->ioTrap & 0x0600))
        d->dg.st.ew_sync_park++;
    mp_diag_ew_out(&d->dg, d->in_tick, r == PENDING, r != noErr && r != PENDING);
    return r;
}
#define ewrite diag_ewrite

/* mp_tx_flush with each frame of the WRITE logged (mp_diag.h, the frame log). */
static int diag_flush(MpDrv *d)
{
    const uint8_t *f[MP_TX_BATCH_MAX];
    uint16_t len[MP_TX_BATCH_MAX], k = mp_ring_used(&d->txr), i;
    int rc;
    if (!d->tx.batch || k == 0)
        k = 1;
    if (k > MP_TX_BATCH_MAX)
        k = MP_TX_BATCH_MAX;
    for (i = 0; i < k; i++)
        f[i] = mp_ring_peek_at(&d->txr, i, &len[i]);
    rc = mp_tx_flush(&d->tx, &d->bus);
    if (rc != MP_E_BUSY)
        for (i = 0; i < k && f[i]; i++)
            mp_diag_wlog(&d->dg, f[i], len[i], rc, i, k, d->scsi.blind_tx);
    return rc;
}
#endif

static int16_t control(MpDrv *d, EPB *pb, void *dce)
{
    switch (pb->csCode) {
    case E_WRITE:
        if (pb->ePointer) {
            const uint8_t *w = (const uint8_t *)pb->ePointer;
            const uint8_t *f = (const uint8_t *)mp_rd32(w + 2);
            if (mp_rd16(w) >= 14 && mp_rd16(f + 12) <= MP_ETYPE_MAXLEN)
                d->calls.write8023++;
        }
        return ewrite(d, pb, dce);
    case E_ATTACH:
        return (int16_t)mp_demux_attach(&d->dm, (uint16_t)pb->eProtType, (void *)pb->ePointer);
    case E_DETACH:
        return (int16_t)mp_demux_detach(&d->dm, (uint16_t)pb->eProtType);
    case E_GETINFO: {
        /* MAC then three zero counters (18 bytes), cut to the caller's buffer. As DaynaPORT,
         * eDataSize is left alone (docs/dev/INTERFACE.md). */
        uint8_t info[18];
        int16_t n = pb->eBuffSize;
        memset(info, 0, sizeof info);
        memcpy(info, d->mac, 6);
        if (n > (int16_t)sizeof info)
            n = sizeof info;
        if (n > 0)
            memcpy((void *)pb->ePointer, info, (size_t)n);
        return noErr;
    }
    case E_ADDMULTI:
        return addmulti(d, pb);
    case MP_CTL_TUNE: {
        const MpTune *u = (const MpTune *)pb->ePointer;
        mp_poll_cfg c;
        if (!u || u->version != MP_TUNE_VERSION)
            return controlErr;
        c.burst_ms = u->burst_ms;
        c.hold_ms = u->hold_ms;
        c.idle_ms = u->idle_ms;
        c.step_ms = u->step_ms;
        c.mid_ms = u->mid_ms;
        c.quiet_ms = u->quiet_ms;
        c.busy_ms = u->busy_ms;
        mp_poll_init(&d->poll, &c, now_ms());   /* clamps out-of-range values */
        d->scsi.blind = u->blind ? 1 : 0;
        d->scsi.blind_tx = u->blind_tx ? 1 : 0;
        d->tx.batch = !(u->flags & MP_TUNE_NOBATCH);
        d->rx_budget = (u->flags & MP_TUNE_RX4) ? 4 : RX_BUDGET;
        d->inline_tx = !(u->flags & MP_TUNE_NOINLINE);
        d->early = u->early;
        d->flags2 = u->flags2;
        d->tflags = u->flags;       /* as set, for Status (MP_TUNE_* minus the old hush widths) */
        DIAG(d->test_spin = u->pad;)
        d->hush = (uint8_t)(u->flags & MP_TUNE_HUSH);
        d->ack_ms = (u->flags & MP_TUNE_ACK2) ? 2 : ACK_MS;
        d->cms = d->ack_ms;
        d->yield = !(u->flags & MP_TUNE_NOYIELD);
        d->chase = (u->flags & MP_TUNE_NOCHASE) ? 0xFF : 0;
        d->verify = u->verify ? 1 : 0;
        return noErr;
    }
#ifdef MP_DIAG
    case MP_CTL_DIAG:
        return mp_diag_control(&d->dg, (const MpDiagCtl *)pb->ePointer);
#endif
    case E_SETGENERAL:              /* MacTCP calls it once after open and needs noErr */
    case E_DELMULTI:
        return noErr;
    default:
        return controlErr;
    }
}

int16_t mp_drv_control(void *vpb, void *dce);
int16_t mp_drv_control(void *vpb, void *dce)
{
    EPB *pb = (EPB *)vpb;
    MpDrv *d = g_d;
    int16_t r;
    if (!d || !d->opened)
        return controlErr;
    r = control(d, pb, dce);
    log_call(d, pb->csCode);
    return r;
}

/* ---- Open / Close / Status / Prime ---- */

int16_t mp_drv_open(void *pb, void *dce);
int16_t mp_drv_open(void *pb, void *dce)
{
    MpDrv *d = g_d;
    uint8_t cdb[6], st[MP_STATS_LEN];
    (void)pb;
    (void)dce;
    if (!d) {
        /* Installed as a DRVR resource in the System file, nobody ran the boot: allocate now. This
         * may be AppleTalk opening us at INIT time, which is fine: nothing here or in the poll
         * reads application globals (docs/dev/BOOT-CRASH-ETHERTALK.md). */
        if (mp_drv_boot(0) != 0)
            return openErr;
        d = g_d;
    }
    if (d->opened)
        return noErr;
    d->scsi.id = mp_scsi_find();
    if (d->scsi.id < 0)
        return openErr;
    mp_scsi_bus(&d->scsi, &d->bus);
    mp_cdb_enable(cdb, 1);
    if (mp_scsi_cmd_in(&d->bus, cdb, 0, 0) != MP_OK)
        return openErr;
    mp_cdb_stats(cdb);
    if (mp_scsi_cmd_in(&d->bus, cdb, st, MP_STATS_LEN) != MP_OK ||
        mp_dev_parse_stats(st, MP_STATS_LEN, d->mac, 0) != MP_OK)
        return openErr;
    mp_ring_reset(&d->rxr);
    mp_ring_reset(&d->txr);
    mp_rx_init(&d->rx, &d->rxr);
    mp_rx_set_full(&d->rx, full_cb, d);
    mp_tx_init(&d->tx, &d->txr, d->mac, 1);   /* batched WRITEs (MoofTune nobatch: one per WRITE) */
    d->rx_budget = RX_BUDGET;
    d->ack_ms = ACK_MS;
    d->cms = ACK_MS;
    d->chase = 0;
    d->yield = 1;
    d->inline_tx = 1;               /* hwsend8: A +27%, C and AppleShare unchanged, no frames lost */
    d->early = MP_EARLY_OFF;        /* hwsend10: the chase's early READs were empty, echo +10 ms */
    d->flags2 = 0;                  /* the reply pause on, no re-poll after a yield cut */
    d->tflags = 0;
    mp_demux_init(&d->dm);
    memset(&d->calls, 0, sizeof d->calls);
    {
        mp_poll_cfg c;
        tune_defaults(&c);
        mp_poll_init(&d->poll, &c, now_ms());
    }
    DIAG(mp_diag_open(&d->dg);)
    DIAG(d->scsi.dg = &d->dg;)
    memset(&d->t, 0, sizeof d->t);
    d->stop = 0;
    d->t[0].tm.tmAddr = d->t[1].tm.tmAddr = (ProcPtr)mp_tm_entry;
    InsTime((QElemPtr)&d->t[0].tm);
    InsTime((QElemPtr)&d->t[1].tm);
    d->opened = 1;
    prime(d, &d->t[0], IDLE_MS);
    d->wdv.qType = vType;
    d->wdv.vblAddr = (ProcPtr)mp_wd_entry;
    d->wdv.vblCount = 6;
    (void)VInstall(&d->wdv);
    return noErr;
}

int16_t mp_drv_close(void *pb, void *dce);
int16_t mp_drv_close(void *pb, void *dce)
{
    MpDrv *d = g_d;
    uint8_t cdb[6];
    (void)pb;
    (void)dce;
    if (!d || !d->opened)
        return noErr;
    d->stop = 1;
    (void)VRemove(&d->wdv);
    RmvTime((QElemPtr)&d->t[0].tm);
    RmvTime((QElemPtr)&d->t[1].tm);
    if (d->parked) {
        d->parked = 0;
        mp_iodone(d->dce, abortErr);
    }
    DIAG(mp_diag_close(&d->dg);)
    mp_cdb_enable(cdb, 0);
    (void)mp_scsi_cmd_in(&d->bus, cdb, 0, 0);
    d->opened = 0;
    return noErr;
}

/* Private Status csCode MP_STATUS_STATS ('MP'): ePointer = a copy of the counters (MpStats,
 * mp_drv.h), eBuffSize = its size. For MoofStat and the Phase 1 runs. */
int16_t mp_drv_status(void *vpb, void *dce);
int16_t mp_drv_status(void *vpb, void *dce)
{
    EPB *pb = (EPB *)vpb;
    MpDrv *d = g_d;
    MpStats *o;
    (void)dce;
#ifdef MP_DIAG
    if ((uint16_t)pb->csCode == MP_STATUS_WLOG && d && pb->ePointer &&
        pb->eBuffSize >= (int16_t)sizeof(MpWLog)) {
        memcpy((void *)pb->ePointer, &d->dg.wl, sizeof(MpWLog));
        pb->eDataSize = (int16_t)sizeof(MpWLog);
        return noErr;
    }
    if ((uint16_t)pb->csCode == MP_STATUS_DIAG && d && pb->ePointer &&
        pb->eBuffSize >= (int16_t)sizeof(MpDiagStats)) {
        memcpy((void *)pb->ePointer, &d->dg.st, sizeof(MpDiagStats));
        pb->eDataSize = (int16_t)sizeof(MpDiagStats);
        return noErr;
    }
#endif
    if ((uint16_t)pb->csCode == MP_STATUS_LOG && d && pb->ePointer &&
        pb->eBuffSize >= (int16_t)sizeof(MpLog)) {
        MpLog *l = (MpLog *)pb->ePointer;
        int i;
        l->version = MP_LOG_VERSION;
        l->c = d->calls;
        for (i = 0; i < 8; i++) {
            l->ph[i].type = d->dm.e[i].type;
            l->ph[i].used = d->dm.e[i].used;
            l->ph[i].has_handler = d->dm.e[i].handler != 0;
            l->ph[i].frames = d->dm.e[i].frames;
        }
        pb->eDataSize = (int16_t)sizeof(MpLog);
        return noErr;
    }
    if ((uint16_t)pb->csCode != MP_STATUS_STATS || !d || !pb->ePointer ||
        pb->eBuffSize < (int16_t)sizeof(MpStats))
        return statusErr;
    o = (MpStats *)pb->ePointer;
    o->version = MP_STATS_VERSION;
    o->scsi_id = d->scsi.id;
    o->opened = d->opened;
    memcpy(o->mac, d->mac, 6);
    o->polls = d->polls;
    o->busy = d->busy;
    o->delivered = d->delivered;
    o->tx_now = d->tx_now;
    o->tx_deferred = d->tx_deferred;
    o->tx_failed = d->tx_failed;
    o->rx = d->rx.st;
    o->tx = d->tx.st;
    o->unattached = d->dm.unattached;
    o->runts = d->dm.runts;
    o->bus_err = d->bus_err;
    o->last_err = d->last_err;
    o->tune.version = MP_TUNE_VERSION;
    o->tune.early = d->early;
    o->tune.flags2 = d->flags2;
    o->tune.burst_ms = d->poll.c.burst_ms;
    o->tune.hold_ms = d->poll.c.hold_ms;
    o->tune.idle_ms = d->poll.c.idle_ms;
    o->tune.step_ms = d->poll.c.step_ms;
    o->tune.mid_ms = d->poll.c.mid_ms;
    o->tune.quiet_ms = d->poll.c.quiet_ms;
    o->tune.busy_ms = d->poll.c.busy_ms;
    o->tune.blind = d->scsi.blind;
    o->tune.verify = d->verify;
    o->tune.blind_tx = d->scsi.blind_tx;
    o->tune.flags = d->tflags;
    o->kicks = d->kicks;
    o->wd = d->wd;
    o->bursts = d->poll.bursts;
    o->immediates = d->poll.immediates;
    o->v_ok = d->v_ok;
    o->v_bad = d->v_bad;
    o->v_big = d->v_big;
    pb->eDataSize = (int16_t)sizeof(MpStats);
    return noErr;
}

int16_t mp_drv_prime(void *pb, void *dce);
int16_t mp_drv_prime(void *pb, void *dce)
{
    (void)pb;
    (void)dce;
    return controlErr;
}

/* ---- boot: called once by the INIT through the boot stub, after relocation ---- */

/* Allocate and set up the resident state in the system heap. Returns 0 on success. The device
 * is not touched here: see Open. Also called by Open when the driver was loaded as a resource. */
long mp_drv_boot(void *drvr);
long mp_drv_boot(void *drvr)
{
    MpDrv *d;
    (void)drvr;
    if (g_d)
        return 0;
    d = (MpDrv *)NewPtrSysClear((Size)sizeof(MpDrv));
    if (!d)
        return -108;               /* memFullErr */
    mp_ring_init(&d->rxr, d->rxmem, RX_SLOTS);
    mp_ring_init(&d->txr, d->txmem, TX_SLOTS);
    d->scsi.blind = 1;             /* decision 5: blind payload reads (hw3 gate passed) */
    d->scsi.blind_tx = 1;          /* hwsend: blind frame writes (hw5: safe) */
    g_d = d;
    return 0;
}

/* mp_diag.h - the hwsend diagnostic build of the MoofPort driver (-DMP_DIAG only; the shipping
 * driver has none of this). What it shares with TcpSend: a private Status call that returns the
 * timing counters below, and a private Control call that resets them or turns the stamps off.
 *
 * Question it answers (hwsend kit, docs/dev/hwsend.md): why MacTCP's sync 1400-byte sends take
 * about one echo round trip each over MoofPort, a little longer than over DaynaPORT. A TCP ACK
 * from the host is delivered to MacTCP's handler inside the poll task, and the handler's next
 * EWrite (the next data segment) is a full SCSI WRITE done right there, nested in deliver().
 *
 * Clock: System 6.0.8 on the Classic has no Microseconds trap (A193; docs/dev/RESEARCH.md
 * section 4). The clock is the revised Time Manager used as a stopwatch, as in ScsiProbe: a task
 * primed with -1,000,000 us, removed with RmvTime to read the time left, then primed again. A
 * read costs about 250 us on the Classic (hw4: lap 205-246 us); lap_us is that cost measured at
 * open. Time between RmvTime and the next PrimeTime is not counted, so the clock runs a little
 * slow (each read loses part of its own cost); stamps are good to about 0.3 ms. If no read comes
 * for a second the task fires and adds the second itself.
 *
 * Histogram bins are 1,024 us wide (us >> 10: no division in the driver); the last bin holds
 * everything from 19.456 ms up. Sums and maxima are in us. */
#ifndef MP_DIAG_H
#define MP_DIAG_H

#include <stdint.h>

#define MP_STATUS_DIAG   0x4D44      /* 'MD' Status: ePointer = MpDiagStats, eBuffSize >= its size */
#define MP_CTL_DIAG      0x4D44      /* 'MD' Control: ePointer = MpDiagCtl */
#define MP_DIAG_VERSION  6           /* 2: send-fix; 3: frame log; 4: EWrite kinds; 5: tick phase;
                                        6: the send cycle */
#define MP_DIAG_BINS     20
#define MP_DIAG_SHIFT    10          /* bin = us >> 10 */

enum {
    MP_DH_DELIV_RET,   /* frame handed to the handler -> the handler's nested EWrite returns */
    MP_DH_NEST_EW,     /* nested EWrite (inside deliver()), entry -> return */
    MP_DH_APP_EW,      /* EWrite outside the poll task (MacTCP at task level), entry -> return */
    MP_DH_KICK,        /* an app EWrite returns -> the next poll tick starts */
    MP_DH_TICK,        /* poll tick, entry -> exit (the clock reads inside it included) */
    MP_DH_ACK_EW,      /* a handler returns (MacTCP has the frame) -> the next app EWrite starts */
    MP_DH_W_BEGIN,     /* WRITE: SCSIGet + SCSISelect + SCSICmd */
    MP_DH_W_DATA,      /* WRITE: preamble, frame(s), trailer */
    MP_DH_W_END,       /* WRITE: SCSIComplete */
    /* version 5: when things happen against the 60 Hz VBL tick (time since the last VBL) */
    MP_DH_PH_DELIV,    /* a handler returns (MacTCP has the frame) */
    MP_DH_PH_EW,       /* an app EWrite starts (MacTCP sends) */
    MP_DH_REPLY,       /* a WRITE ends -> the next frame is handed to a handler (the reply wait) */
    /* version 6: the send cycle (hwsend7), one per round trip: from an app EWrite to the first app
     * EWrite after a frame was delivered and its tick returned. Further app EWrites in between
     * (a burst) stay in the cycle. Each stage is counted once per cycle, the first time. */
    MP_DH_C_EW_W,      /* app EWrite entry -> the WRITE starts (inline: in the call; else the tick) */
    MP_DH_C_WRITE,     /* that WRITE, start -> end */
    MP_DH_C_W_READ,    /* WRITE end -> start of the poll tick that delivers the first frame after it */
    MP_DH_C_READ,      /* that tick's start -> the frame handed to the handler (its READ(s)) */
    MP_DH_C_HANDLER,   /* handler entry -> return (MacTCP's handler, inside our tick) */
    MP_DH_C_TICKRET,   /* handler return -> our tick returns */
    MP_DH_C_MACTCP,    /* our tick returns -> the next app EWrite starts (MacTCP and the app, plus
                          anything of ours in between: MP_DH_C_CPU_POST) */
    MP_DH_C_TOTAL,     /* the cycle: app EWrite entry -> the next cycle's first app EWrite entry */
    MP_DH_C_CPU,       /* our interrupt-level time in the cycle: poll ticks + app EWrite calls */
    MP_DH_C_CPU_POST,  /* ... of it after the delivering tick returned (taken from MacTCP's time) */
    MP_DH_N
};

typedef struct MpDiagHist {
    uint32_t n, sum_us, max_us;
    uint32_t bin[MP_DIAG_BINS];
} MpDiagHist;

typedef struct MpDiagStats {
    uint16_t version;
    uint8_t  stamps;          /* 1 = clock reads on (the default); 0 = counts only */
    uint8_t  pad;
    uint32_t lap_us;          /* one clock read, measured at open (min of 8) */
    uint32_t clk_reads, clk_fires;
    uint32_t ticks, ticks_rx; /* poll ticks; ... that delivered at least one frame */
    uint32_t delivered;       /* frames handed to a handler */
    uint32_t deliv_ew;        /* ... whose handler called EWrite before returning */
    uint32_t ew_nested;       /* EWrites from inside deliver() */
    uint32_t ew_nested_q;     /* ... queued and completed at once */
    uint32_t ew_nested_park;  /* ... left pending (both TX slots full) */
    uint32_t ew_tick_other;   /* EWrites in the poll task but not in deliver(): since send-fix,
                                 the next queued call, started by IODone of a pending one */
    uint32_t ew_app;          /* EWrites outside the poll task */
    uint32_t ew_app_q, ew_app_park;
    uint32_t ew_err;          /* EWrites that returned an error */
    uint32_t ew_app_irq;      /* app EWrites made at interrupt level (SR mask > 0): MacTCP's own
                                 timer or completion routines, not its task-level code */
    MpDiagHist h[MP_DH_N];
    /* version 4: EWrites by kind, [async, sync, immediate] x [task level, interrupt level] */
    uint32_t ew_kind[6];
    uint32_t ew_refused;      /* refused with controlErr: ring full and no WRITE possible */
    uint32_t ew_sync_park;    /* sync calls left pending (task level, ring full) */
    /* version 6: per send cycle (MP_DH_C_*), sums over all cycles (divide by h[MP_DH_C_TOTAL].n) */
    uint32_t c_reads;         /* READs started */
    uint32_t c_reads_post;    /* ... after the delivering tick returned */
    uint32_t c_ticks;         /* poll ticks */
    uint32_t c_ticks_post;    /* ... after the delivering tick returned */
    uint32_t c_clk;           /* clock reads (each costs about lap_us, counted in the times) */
    uint32_t c_vbl;           /* our VBL task runs (one clock read each, not in MP_DH_C_CPU) */
    uint32_t c_ew_more;       /* further app EWrites inside a cycle (sum, not per cycle) */
    uint32_t c_writes;        /* WRITEs started */
} MpDiagStats;

/* The frame log (send-fix's frame-loss hunt): every frame of every WRITE, newest last, in a ring
 * of MP_WLOG_N. Status MP_STATUS_WLOG copies it out; the diag reset empties it. */
#define MP_STATUS_WLOG   0x4D57      /* 'MW' Status: ePointer = MpWLog, eBuffSize >= its size */
#define MP_WLOG_N        256

typedef struct MpWEntry {
    uint32_t seq;             /* TCP: sequence number; DDP: dst net << 16 | dst node << 8 | dst socket */
    uint16_t t;               /* stopwatch at the WRITE's end, in 1.024 ms units (low 16 bits) */
    uint16_t etype;           /* EtherType; 0x809B = AppleTalk DDP in 802.3 SNAP; else the 802.3 length */
    uint16_t id;              /* IPv4: IP ID; DDP: src socket << 8 | DDP type */
    uint16_t len;             /* TCP: payload bytes; DDP: DDP length; else the frame length */
    uint16_t port;            /* TCP: destination port; ATP: transaction ID */
    int8_t   rc;              /* mp_tx_flush: frames the WRITE sent (> 0), or an MP_E_* error */
    uint8_t  pos;             /* bits 0-2: index in the WRITE, 3-6: frames in it, 7: blind */
    uint8_t  proto;           /* IPv4 protocol (6 = TCP), ATP control byte for DDP type 3 */
    uint8_t  pad[3];
} MpWEntry;                   /* 20 bytes */

typedef struct MpWLog {
    uint16_t version;         /* MP_DIAG_VERSION */
    uint16_t pos;             /* next entry to write */
    uint32_t total;           /* entries written since the reset (> MP_WLOG_N: the oldest are gone) */
    MpWEntry e[MP_WLOG_N];
} MpWLog;

enum { MP_DIAG_OP_NONE = 0, MP_DIAG_OP_RESET = 1 };

typedef struct MpDiagCtl {
    uint16_t version;         /* MP_DIAG_VERSION */
    uint8_t  op;              /* MP_DIAG_OP_* */
    uint8_t  stamps;          /* 1 = clock reads on, 0 = off (counts only) */
} MpDiagCtl;

#ifdef MP_DIAG_DRIVER
/* Driver side (mp_diag.c). The clock task must come first: its entry gets the TMTask in A1. */
#include <Timer.h>
#include <OSUtils.h>          /* VBLTask, VInstall (Multiverse) */

typedef struct MpDiag {
    TMTask   clk;
    uint32_t acc;             /* us counted so far */
    uint8_t  on;              /* the clock task is installed */
    uint8_t  in_deliver;      /* a handler is running (deliver()) */
    uint8_t  deliv_wrote;     /* ... and it called EWrite */
    uint8_t  kick_pending;    /* an app EWrite returned since the last tick started */
    uint8_t  tick_rx;
    uint8_t  pad[3];
    uint32_t t_tick, t_deliv, t_ew, t_app_ret, t_dret, t_w;
    uint8_t  dret_pending;    /* a handler returned since the last app EWrite */
    uint8_t  wr;              /* the transaction in progress is a WRITE */
    uint8_t  reply_pending;   /* a WRITE ended and no frame was delivered since */
    uint8_t  pad2;
    uint32_t t_vbl, t_wend;   /* the last VBL tick; the last WRITE's end */
    /* the send cycle (version 6) */
    uint8_t  c_on;            /* a cycle is open (an app EWrite came) */
    uint8_t  c_stage;         /* 1 WRITE started, 2 WRITE ended, 3 delivered, 4 handler returned,
                                 5 the delivering tick returned */
    uint8_t  pad3[2];
    uint32_t c_t0, c_t, c_clk0;         /* cycle start; the last stage; clock reads at the start */
    uint32_t c_cpu, c_post;   /* our interrupt-level time in the cycle; ... after stage 5 */
    struct MpDiagVbl { VBLTask v; struct MpDiag *g; } vbl;
    MpDiagStats st;
    MpWLog   wl;
} MpDiag;

void mp_diag_open(MpDiag *g);
void mp_diag_close(MpDiag *g);
void mp_diag_tick_in(MpDiag *g);
void mp_diag_tick_out(MpDiag *g);
void mp_diag_deliver_in(MpDiag *g);
void mp_diag_deliver_out(MpDiag *g);
void mp_diag_ew_in(MpDiag *g, int in_tick);
/* in_tick: called from the poll task; parked: left pending; err: an error result */
void mp_diag_ew_out(MpDiag *g, int in_tick, int parked, int err);
/* mp_scsi.c, around each transaction: begin (cdb), after begin, before end, after end */
void mp_diag_bus(MpDiag *g, int step, const uint8_t *cdb);
int16_t mp_diag_control(MpDiag *g, const MpDiagCtl *c);
/* After a WRITE: frame f (n bytes) was index i of the k frames of a WRITE that returned rc. */
void mp_diag_wlog(MpDiag *g, const uint8_t *f, uint16_t n, int rc, int i, int k, int blind);
#endif

#endif

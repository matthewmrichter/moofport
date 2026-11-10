/* mp_diag.c - the hwsend diagnostic build's timing counters (mp_diag.h). Linked only into
 * MoofPortDiagDrv (-DMP_DIAG); the shipping driver has none of this.
 *
 * Every hook is called from mp_drv.c: the poll tick (Time Manager, interrupt level), deliver()
 * around each handler call, and the EWrite Control call (task level from MacTCP, or nested
 * inside deliver()). The clock read masks interrupts, so a read from task level cannot be cut by
 * the poll tick or the clock task's own firing. */
#include <Timer.h>
#include <string.h>
#define MP_DIAG_DRIVER
#include "mp_diag.h"

#define CLK_US 1000000L

extern void mp_diag_clk_entry(void);   /* mp_diag_s.s: A1 = the TMTask -> mp_diag_clk_fire */
extern void mp_diag_vbl_entry(void);   /* mp_diag_s.s: A0 = the VBLTask -> mp_diag_vbl_fire */

/* No read for a second: count it and go round again. */
void mp_diag_clk_fire(MpDiag *g);
void mp_diag_clk_fire(MpDiag *g)
{
    g->acc += (uint32_t)CLK_US;
    g->st.clk_fires++;
    PrimeTime((QElemPtr)&g->clk, -CLK_US);
}

static uint32_t clk(MpDiag *g)
{
    uint16_t sr;
    uint32_t t;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    RmvTime((QElemPtr)&g->clk);
    g->acc += (uint32_t)(CLK_US + g->clk.tmCount);   /* tmCount: us left, negative */
    t = g->acc;
    g->clk.tmCount = 0;
    InsTime((QElemPtr)&g->clk);
    PrimeTime((QElemPtr)&g->clk, -CLK_US);
    g->st.clk_reads++;
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
    return t;
}

static void hist(MpDiagHist *h, uint32_t us);

/* Every 60 Hz VBL interrupt: note the time, for the phase histograms. */
void mp_diag_vbl_fire(struct MpDiagVbl *v);
void mp_diag_vbl_fire(struct MpDiagVbl *v)
{
    v->v.vblCount = 1;
    if (v->g->st.stamps) {
        v->g->t_vbl = clk(v->g);
        if (v->g->c_on)
            v->g->st.c_vbl++;
    }
}

static void hist(MpDiagHist *h, uint32_t us)
{
    uint32_t b = us >> MP_DIAG_SHIFT;
    h->n++;
    h->sum_us += us;
    if (us > h->max_us)
        h->max_us = us;
    h->bin[b < MP_DIAG_BINS ? b : MP_DIAG_BINS - 1]++;
}

/* The send cycle: stage k (1..5) happened at t; its time since the previous stage goes into
 * histogram h. Only the first time in a cycle, and only in order. */
static void stage(MpDiag *g, uint8_t k, int h, uint32_t t)
{
    if (!g->c_on || g->c_stage != k - 1)
        return;
    hist(&g->st.h[h], t - g->c_t);
    g->c_stage = k;
    g->c_t = t;
}

static void reset(MpDiag *g)
{
    uint32_t lap = g->st.lap_us;
    uint8_t stamps = g->st.stamps;
    memset(&g->st, 0, sizeof g->st);
    g->st.version = MP_DIAG_VERSION;
    g->st.lap_us = lap;
    g->st.stamps = stamps;
    g->kick_pending = 0;
    g->dret_pending = 0;
    g->c_on = 0;
    g->wl.version = MP_DIAG_VERSION;
    g->wl.pos = 0;
    g->wl.total = 0;
}

void mp_diag_open(MpDiag *g)
{
    uint32_t a, b, best = 0xFFFFFFFFu;
    int i;
    if (g->on)
        return;
    memset(g, 0, sizeof *g);
    g->clk.tmAddr = (ProcPtr)mp_diag_clk_entry;
    InsTime((QElemPtr)&g->clk);
    PrimeTime((QElemPtr)&g->clk, -CLK_US);
    g->on = 1;
    g->vbl.g = g;
    g->vbl.v.qType = vType;
    g->vbl.v.vblAddr = (ProcPtr)mp_diag_vbl_entry;
    g->vbl.v.vblCount = 1;
    VInstall(&g->vbl.v);
    for (i = 0; i < 8; i++) {
        a = clk(g);
        b = clk(g);
        if (b - a < best)
            best = b - a;
    }
    g->st.stamps = 1;
    g->st.lap_us = best;
    reset(g);
}

void mp_diag_close(MpDiag *g)
{
    if (!g->on)
        return;
    VRemove(&g->vbl.v);
    RmvTime((QElemPtr)&g->clk);
    g->on = 0;
}

void mp_diag_tick_in(MpDiag *g)
{
    g->st.ticks++;
    g->tick_rx = 0;
    if (!g->st.stamps)
        return;
    g->t_tick = clk(g);
    if (g->kick_pending) {
        hist(&g->st.h[MP_DH_KICK], g->t_tick - g->t_app_ret);
        g->kick_pending = 0;
    }
}

void mp_diag_tick_out(MpDiag *g)
{
    uint32_t t, us;
    if (g->tick_rx)
        g->st.ticks_rx++;
    if (!g->st.stamps)
        return;
    t = clk(g);
    us = t - g->t_tick;
    hist(&g->st.h[MP_DH_TICK], us);
    if (g->c_on) {
        g->c_cpu += us;
        g->st.c_ticks++;
        if (g->c_stage == 5) {
            g->c_post += us;
            g->st.c_ticks_post++;
        }
        if (g->c_stage == 4) {
            hist(&g->st.h[MP_DH_C_TICKRET], t - g->c_t);
            g->c_stage = 5;
            g->c_t = t;
        }
    }
}

void mp_diag_deliver_in(MpDiag *g)
{
    g->in_deliver = 1;
    g->deliv_wrote = 0;
    g->tick_rx = 1;
    g->st.delivered++;
    if (g->st.stamps) {
        g->t_deliv = clk(g);
        if (g->c_on && g->c_stage == 2) {
            hist(&g->st.h[MP_DH_C_W_READ], g->t_tick - g->c_t);
            hist(&g->st.h[MP_DH_C_READ], g->t_deliv - g->t_tick);
            g->c_stage = 3;
            g->c_t = g->t_deliv;
        }
    }
}

void mp_diag_deliver_out(MpDiag *g)
{
    if (g->deliv_wrote)
        g->st.deliv_ew++;
    g->in_deliver = 0;
    if (g->st.stamps) {
        g->t_dret = clk(g);
        g->dret_pending = 1;
        stage(g, 4, MP_DH_C_HANDLER, g->t_dret);
        hist(&g->st.h[MP_DH_PH_DELIV], g->t_dret - g->t_vbl);
        if (g->reply_pending) {
            hist(&g->st.h[MP_DH_REPLY], g->t_dret - g->t_wend);
            g->reply_pending = 0;
        }
    }
}

void mp_diag_ew_in(MpDiag *g, int in_tick)
{
    if (!g->st.stamps)
        return;
    g->t_ew = clk(g);
    if (!in_tick)
        hist(&g->st.h[MP_DH_PH_EW], g->t_ew - g->t_vbl);
    if (!in_tick && g->c_on && g->c_stage != 5) {
        g->st.c_ew_more++;            /* a further app EWrite in the same cycle (a burst) */
    } else if (!in_tick) {
        /* The first app EWrite after a delivering tick returned closes the cycle and starts the
         * next one: one cycle per round trip, however many segments MacTCP sends in it. */
        if (g->c_on) {
            hist(&g->st.h[MP_DH_C_MACTCP], g->t_ew - g->c_t);
            hist(&g->st.h[MP_DH_C_TOTAL], g->t_ew - g->c_t0);
            hist(&g->st.h[MP_DH_C_CPU], g->c_cpu);
            hist(&g->st.h[MP_DH_C_CPU_POST], g->c_post);
            g->st.c_clk += g->st.clk_reads - g->c_clk0;
        }
        g->c_on = 1;
        g->c_stage = 0;
        g->c_t0 = g->c_t = g->t_ew;
        g->c_cpu = g->c_post = 0;
        g->c_clk0 = g->st.clk_reads;
    }
    if (!in_tick && g->dret_pending) {
        hist(&g->st.h[MP_DH_ACK_EW], g->t_ew - g->t_dret);
        g->dret_pending = 0;
    }
}

/* step 0: begin starts (cdb[0] 0x0A = WRITE), 1: begin done, 2: end starts, 3: end done. */
void mp_diag_bus(MpDiag *g, int step, const uint8_t *cdb)
{
    uint32_t t;
    if (step == 0) {
        g->wr = cdb[0] == 0x0A;
        if (cdb[0] == 0x08 && g->c_on) {      /* a READ: counted, not stamped (no extra clock reads) */
            g->st.c_reads++;
            if (g->c_stage == 5)
                g->st.c_reads_post++;
        }
    }
    if (!g->wr || !g->st.stamps)
        return;
    t = clk(g);
    if (step == 0 && g->c_on)
        g->st.c_writes++;
    if (step == 0)
        stage(g, 1, MP_DH_C_EW_W, t);
    if (step == 3)
        stage(g, 2, MP_DH_C_WRITE, t);
    if (step)
        hist(&g->st.h[step == 1 ? MP_DH_W_BEGIN : step == 2 ? MP_DH_W_DATA : MP_DH_W_END],
             t - g->t_w);
    if (step == 3) {
        g->t_wend = t;
        g->reply_pending = 1;
    }
    g->t_w = t;
}

void mp_diag_ew_out(MpDiag *g, int in_tick, int parked, int err)
{
    uint32_t t = g->st.stamps ? clk(g) : 0;
    if (err)
        g->st.ew_err++;
    if (in_tick && g->in_deliver) {
        g->st.ew_nested++;
        if (parked)
            g->st.ew_nested_park++;
        else if (!err)
            g->st.ew_nested_q++;
        g->deliv_wrote = 1;
        if (g->st.stamps) {
            hist(&g->st.h[MP_DH_DELIV_RET], t - g->t_deliv);
            hist(&g->st.h[MP_DH_NEST_EW], t - g->t_ew);
        }
    } else if (in_tick) {
        g->st.ew_tick_other++;
    } else {
        uint16_t sr;
        __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
        g->st.ew_app++;
        if (sr & 0x0700)
            g->st.ew_app_irq++;
        if (parked)
            g->st.ew_app_park++;
        else if (!err)
            g->st.ew_app_q++;
        if (g->st.stamps) {
            hist(&g->st.h[MP_DH_APP_EW], t - g->t_ew);
            g->c_cpu += t - g->t_ew;
            g->t_app_ret = t;
            g->kick_pending = 1;
        }
    }
}

int16_t mp_diag_control(MpDiag *g, const MpDiagCtl *c)
{
    if (!c || c->version != MP_DIAG_VERSION)
        return -17;                 /* controlErr */
    g->st.stamps = c->stamps ? 1 : 0;
    if (c->op == MP_DIAG_OP_RESET)
        reset(g);
    return 0;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }

void mp_diag_wlog(MpDiag *g, const uint8_t *f, uint16_t n, int rc, int i, int k, int blind)
{
    MpWEntry *e = &g->wl.e[g->wl.pos];
    uint16_t et = be16(f + 12);
    memset(e, 0, sizeof *e);
    e->t = (uint16_t)((g->st.stamps ? clk(g) : g->acc) >> MP_DIAG_SHIFT);
    e->etype = et;
    e->len = n;
    e->rc = (int8_t)rc;
    e->pos = (uint8_t)((i & 7) | (k & 15) << 3 | (blind ? 0x80 : 0));
    if (et == 0x0800 && n >= 34) {
        const uint8_t *ip = f + 14, *t;
        uint16_t ihl = (uint16_t)((ip[0] & 15) * 4);
        e->proto = ip[9];
        e->id = be16(ip + 4);
        t = ip + ihl;
        if (ip[9] == 6 && n >= 14 + ihl + 20) {
            e->port = be16(t + 2);
            e->seq = (uint32_t)be16(t + 4) << 16 | be16(t + 6);
            e->len = (uint16_t)(be16(ip + 2) - ihl - (t[12] >> 4) * 4);
        }
    } else if (et <= 1500 && n >= 14 + 8 + 13 && f[14] == 0xAA && f[15] == 0xAA &&
               be16(f + 20) == 0x809B) {
        const uint8_t *d = f + 22;           /* long DDP header */
        e->etype = 0x809B;
        e->len = (uint16_t)((d[0] & 3) << 8 | d[1]);
        e->seq = (uint32_t)be16(d + 4) << 16 | (uint32_t)d[8] << 8 | d[10];
        e->id = (uint16_t)(d[11] << 8 | d[12]);
        if (d[12] == 3 && n >= 14 + 8 + 13 + 4) {   /* ATP: control byte, bitmap/seq, TID */
            e->proto = d[13];
            e->port = be16(d + 15);
        }
    }
    g->wl.pos = (uint16_t)((g->wl.pos + 1) & (MP_WLOG_N - 1));
    g->wl.total++;
}

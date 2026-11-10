/* test_poll.c: adaptive poll-rate state machine (PLAN 2.2). */
#include "t.h"
#include "mp_poll.h"

static void test_defaults_start_idle(void)
{
    mp_poll p;
    mp_poll_init(&p, NULL, 1000);
    T_EQ(p.c.burst_ms, MP_POLL_BURST_MS);
    T_EQ(p.c.idle_ms, MP_POLL_IDLE_MS);
    T_EQ(mp_poll_after(&p, 1017, 0), MP_POLL_IDLE_MS);   /* idle stays at the ceiling */
}

static void test_burst_hold_backoff_double(void)
{
    mp_poll p;
    uint32_t t = 5000;
    mp_poll_init(&p, NULL, t);
    T_EQ(mp_poll_after(&p, t, MP_PE_RX), 3);
    t += 3;
    T_EQ(mp_poll_after(&p, t, 0), 3);       /* within hold: stay at burst */
    t += 46;
    T_EQ(mp_poll_after(&p, t, 0), 3);       /* 49 ms since activity */
    t += 3;
    T_EQ(mp_poll_after(&p, t, 0), 6);       /* hold over: back off */
    t += 6;
    T_EQ(mp_poll_after(&p, t, 0), 12);
    t += 12;
    T_EQ(mp_poll_after(&p, t, 0), 17);      /* ceiling */
    t += 17;
    T_EQ(mp_poll_after(&p, t, 0), 17);
    T_EQ(mp_poll_after(&p, t, MP_PE_TX), 3); /* TX also bursts */
}

static void test_step_add(void)
{
    mp_poll p;
    mp_poll_cfg c = { 2, 10, 9, 3, 1, 0, 0 };
    mp_poll_init(&p, &c, 0);
    T_EQ(mp_poll_after(&p, 0, MP_PE_RX), 2);
    T_EQ(mp_poll_after(&p, 10, 0), 5);
    T_EQ(mp_poll_after(&p, 15, 0), 8);
    T_EQ(mp_poll_after(&p, 23, 0), 9);
}

static void test_more_and_busy(void)
{
    mp_poll p;
    mp_poll_init(&p, NULL, 0);
    T_EQ(mp_poll_after(&p, 0, MP_PE_RX | MP_PE_MORE), 0);   /* poll again at once */
    T_EQ(p.cur, 3);                                          /* and burst after that */
    T_EQ(mp_poll_after(&p, 1, MP_PE_BUSY), MP_POLL_BUSY_MS);
    T_EQ(p.cur, 3);                                          /* busy keeps the rate */
    T_EQ(p.immediates, 1);
    T_EQ(p.busies, 1);
    T_EQ(mp_poll_after(&p, 2, MP_PE_MORE), 0);              /* more alone (old fw) */
}

static void test_kick(void)
{
    mp_poll p;
    mp_poll_init(&p, NULL, 0);
    T_EQ(mp_poll_kick(&p, 100), 3);
    T_EQ(mp_poll_after(&p, 120, 0), 3);
}

static void test_clock_wrap(void)
{
    mp_poll p;
    uint32_t t = 0xFFFFFFF0UL;
    mp_poll_init(&p, NULL, t);
    T_EQ(mp_poll_after(&p, t, MP_PE_RX), 3);
    t += 40;                                   /* wrapped past 0 */
    T_EQ(mp_poll_after(&p, t, 0), 3);          /* still within hold */
    t += 20;
    T_EQ(mp_poll_after(&p, t, 0), 6);
}

static void test_clamp_cfg(void)
{
    mp_poll p;
    mp_poll_cfg c = { 0, 0, 0, 0, 0, 0, 0 };
    mp_poll_init(&p, &c, 0);
    T_EQ(p.c.burst_ms, 1);
    T_EQ(p.c.idle_ms, 1);
    T_EQ(p.c.busy_ms, 1);
    T_EQ(mp_poll_after(&p, 5, 0), 1);
}

static void test_mid_level(void)
{
    mp_poll p;
    mp_poll_cfg c = { 5, 60, 51, 17, 2, 17, 2000 };   /* MoofPort 1.0 defaults */
    uint32_t t = 100000;
    mp_poll_init(&p, &c, t);
    T_EQ(mp_poll_after(&p, t, 0), 51);           /* nothing seen yet: idle ceiling */
    T_EQ(mp_poll_after(&p, t, MP_PE_RX), 5);     /* traffic: burst */
    T_EQ(mp_poll_after(&p, t + 30, 0), 5);       /* within the 60 ms hold */
    T_EQ(mp_poll_after(&p, t + 61, 0), 17);      /* hold over: one tick */
    T_EQ(mp_poll_after(&p, t + 1999, 0), 17);    /* still under 2 s of quiet */
    T_EQ(mp_poll_after(&p, t + 2001, 0), 34);    /* quiet: step out */
    T_EQ(mp_poll_after(&p, t + 2035, 0), 51);
    T_EQ(mp_poll_after(&p, t + 2086, 0), 51);    /* ceiling */
    T_EQ(mp_poll_after(&p, t + 2100, MP_PE_TX), 5);
    T_EQ(mp_poll_kick(&p, t + 3000), 5);         /* EWrite outside the poll: burst */
}

static void test_mid_level_doubling_and_clamp(void)
{
    mp_poll p;
    mp_poll_cfg c = { 4, 10, 40, 0, 1, 50, 5 };  /* mid above idle, quiet under hold */
    mp_poll_init(&p, &c, 0);
    T_EQ(p.c.mid_ms, 40);
    T_EQ(p.c.quiet_ms, 10);
    c.mid_ms = 8;
    c.quiet_ms = 100;
    mp_poll_init(&p, &c, 0);
    T_EQ(mp_poll_after(&p, 0, MP_PE_RX), 4);
    T_EQ(mp_poll_after(&p, 11, 0), 8);           /* mid */
    T_EQ(mp_poll_after(&p, 101, 0), 16);         /* doubling from mid */
    T_EQ(mp_poll_after(&p, 117, 0), 32);
    T_EQ(mp_poll_after(&p, 149, 0), 40);
}

T_MAIN(test_mid_level, test_mid_level_doubling_and_clamp, test_defaults_start_idle, test_burst_hold_backoff_double, test_step_add,
       test_more_and_busy, test_kick, test_clock_wrap, test_clamp_cfg)

/* mp_levers.h - the MP_CTL_TUNE settings MoofTune (presets named by the run id) and MoofBatch
 * (its "settings" line) both offer, in one place so the two stay the same. Header only, no
 * Toolbox; tests/test_levers.c checks every name. Needs mp_drv.h first.
 *
 *   new        the defaults (MoofPort 1.1: steadyack, mp_tune_boot in mp_drv.h)
 *   old        MoofPort 1.0's defaults (5 ms bursts, 17/51 ms ticks, 5 ms after a WRITE)
 *   rx4        the 1.0 defaults, but up to 4 received frames per poll tick
 *   chase1     the 1.0 defaults, but a poll right after an inline WRITE, then every 1 ms
 *   steady     DaynaPORT's rhythm: a poll every 16 ms, one frame per READ, no yield cut, chase,
 *              pause or early poll after an inline WRITE
 *   steadyrx4  steady with a budget of 4 received frames per tick
 *   steadyack  the same as new: steady, but one quick poll 2 ms after an inline WRITE (MoofTune ack=2). The tune
 *              ABI has no way to say "steady and that poll": the driver ties the early poll's
 *              suppression to MP_TUNE2_STEADY, which also means one frame per READ. So
 *              steadyack leaves MP_TUNE2_STEADY off and keeps the rest of steady: up to the
 *              budget of frames per READ, cut by nothing (NOYIELD), and the 16 ms rhythm.
 */
#ifndef MP_LEVERS_H
#define MP_LEVERS_H

#include <string.h>

/* MoofTune's and MoofBatch's defaults: what the driver starts with (mp_tune_boot, steadyack). */
static void mp_tune_defaults(MpTune *t)
{
    mp_tune_boot(t);
}

/* MoofPort 1.0's defaults. Every older preset ("the defaults but ...", oldhush, noinline, ...) is
 * built on these, so a result from before 1.1 still means what it did. */
static void mp_tune_old(MpTune *t)
{
    t->burst_ms = 5; t->hold_ms = 60; t->mid_ms = 17; t->quiet_ms = 2000; t->idle_ms = 51;
    t->step_ms = 17; t->busy_ms = 2;
    t->blind = 1; t->verify = 0; t->blind_tx = 1; t->flags = 0; t->early = MP_EARLY_OFF; t->pad = 0;
    t->flags2 = 0; t->pad2 = 0;
}

static void mp_tune_steady(MpTune *t)
{
    t->burst_ms = t->mid_ms = t->idle_ms = 16; t->step_ms = 0;
    t->flags = MP_TUNE_NOYIELD | MP_TUNE_NOCHASE;
    t->flags2 = MP_TUNE2_NOPAUSE | MP_TUNE2_STEADY;
}

/* Sets every field. Returns 1 if the name is one of the levers above, else 0 (t untouched). */
static int mp_lever_preset(MpTune *t, const char *name)
{
    if (!strcmp(name, "new") || !strcmp(name, "steadyack")) { mp_tune_defaults(t); return 1; }
    if (!strcmp(name, "old")) { mp_tune_old(t); return 1; }
    if (!strcmp(name, "rx4")) { mp_tune_old(t); t->flags |= MP_TUNE_RX4; return 1; }
    if (!strcmp(name, "chase1")) { mp_tune_old(t); t->early = MP_EARLY_1; return 1; }
    if (!strcmp(name, "steady")) { mp_tune_old(t); mp_tune_steady(t); return 1; }
    if (!strcmp(name, "steadyrx4")) { mp_tune_old(t); mp_tune_steady(t); t->flags |= MP_TUNE_RX4; return 1; }
    return 0;
}

#endif

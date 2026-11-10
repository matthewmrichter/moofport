/* cpumeter.c - CpuMeter: a busy-loop counter that reports loops/s.
 *
 * Phase 0 baseline kit (docs/dev/PLAN.md section 4, step 5): "CPU left over (a busy-loop counter
 * app) while idle and during transfer". Run it with nothing else happening to get the
 * idle loops/s, then run it again while TcpBench moves data (or, on a non-MultiFinder
 * System 6, run it first, note its last sample, then start the transfer). A lower loops/s
 * during traffic is the CPU the driver + stack are taking.
 *
 * It spins a trivial counter and samples the count at each tick boundary (TickCount, low
 * memory 0x16A) for a window of SECS seconds, reporting per-second loops/s samples, the
 * mean and the min. No network, no Memory Manager in the loop, no MultiFinder assumptions.
 * Results go to an alert and to CpuMeter-<run id>.out next to the app (runid.c).
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Memory.h>
#include <Files.h>
#include <OSUtils.h>
#include <stdio.h>
#include <string.h>
#include "runid.h"
#include <stdint.h>

QDGlobals qd;

#define SECS 10                 /* measurement window, seconds */

/* Ticks (60.15 Hz) from the low-memory global; *(volatile unsigned long*)0x16A. */
static unsigned long ticks(void) { return *(volatile unsigned long *)0x16A; }

static short g_outref = -1;
static Str255 g_outname;      /* "<Tool>-<run id>.out", see runid.c */
static void wr(const char *s)
{
    long n = (long)strlen(s);
    if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s);
}

int main(void)
{
    char line[256];
    Str255 ps;
    size_t n;
    int i;
    volatile uint32_t counter = 0;
    uint32_t prev, cur;
    unsigned long t_end, t_next;
    uint32_t samples[SECS];
    uint32_t sum = 0, mn = 0xFFFFFFFFu;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
    runid_name("CpuMeter", ".out", g_outname);

    FSDelete((ConstStr255Param)g_outname, 0);
    Create((ConstStr255Param)g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStr255Param)g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
    wr("CpuMeter\r");

    /* Align to a tick boundary, then one-second windows of 60 (approx 60.15) ticks. */
    t_next = ticks();
    while (ticks() == t_next) { }       /* wait for the next tick edge */
    for (i = 0; i < SECS; i++) {
        prev = counter;
        t_end = ticks() + 60;           /* ~1 s (60 ticks ~ 0.998 s) */
        while (ticks() < t_end) { counter++; }
        cur = counter;
        samples[i] = cur - prev;
        sum += samples[i];
        if (samples[i] < mn) mn = samples[i];
    }

    for (i = 0; i < SECS; i++) {
        sprintf(line, "second %2d: %lu loops\r", i + 1, (unsigned long)samples[i]);
        wr(line);
    }
    sprintf(line, "mean %lu loops/s, min %lu loops/s over %d s\r",
            (unsigned long)(sum / SECS), (unsigned long)mn, SECS);
    wr(line);

    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);

    sprintf(line, "CpuMeter: mean %lu loops/s, min %lu loops/s (%d s). See %.*s.",
            (unsigned long)(sum / SECS), (unsigned long)mn, SECS, (int)g_outname[0], (const char *)g_outname + 1);
    n = strlen(line);
    if (n > 255) n = 255;
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, line, n);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(128, 0);
    return 0;
}

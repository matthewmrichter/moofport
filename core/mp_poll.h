/* mp_poll.h: adaptive poll-rate state machine (PLAN 2.2).
 *
 * The device never interrupts or reselects, so the driver polls it from a
 * Time Manager task. After a poll that received frames, or after a frame was
 * sent, the next poll comes 'burst_ms' later, and stays at that rate for
 * 'hold_ms' after the last activity. After that each idle poll grows the
 * interval (doubling when step_ms is 0, else adding step_ms) up to 'idle_ms'.
 * A device that says more is queued is polled again at once (0 ms). A busy
 * bus is retried after 'busy_ms' without changing the rate.
 *
 * Optional middle level (mid_ms != 0, 1.0 defaults): after hold_ms the
 * interval is mid_ms until quiet_ms after the last activity, and only then
 * grows towards idle_ms. MoofPort 1.0: 5 ms for 60 ms after traffic, one tick
 * until 2 s of quiet, then one more tick per poll up to three.
 *
 * All times are ms on a uint32 clock compared wrap-safely. The defaults are a
 * starting point; real values come from the Phase 4 hardware sweep.
 */
#ifndef MP_POLL_H
#define MP_POLL_H

#include "mp_util.h"

typedef struct mp_poll_cfg {
    uint16_t burst_ms;     /* interval while active (PLAN: about 2-4 ms) */
    uint16_t hold_ms;      /* stay at burst this long after the last activity */
    uint16_t idle_ms;      /* ceiling when idle (PLAN: initially 1 tick) */
    uint16_t step_ms;      /* idle growth per poll; 0 = double */
    uint16_t busy_ms;      /* retry delay when the bus was busy */
    uint16_t mid_ms;       /* 0 = off; else the interval from hold_ms to quiet_ms */
    uint16_t quiet_ms;     /* with mid_ms: back off towards idle_ms only after this */
} mp_poll_cfg;

#define MP_POLL_BURST_MS  3u
#define MP_POLL_HOLD_MS   50u
#define MP_POLL_IDLE_MS   17u    /* about one 60.15 Hz tick */
#define MP_POLL_STEP_MS   0u
#define MP_POLL_BUSY_MS   2u

/* Event bits for mp_poll_after. */
#define MP_PE_RX    0x01u   /* the poll delivered at least one frame */
#define MP_PE_TX    0x02u   /* a frame was sent (or queued for the next poll) */
#define MP_PE_MORE  0x04u   /* the device has more queued (mp_rx_end said so) */
#define MP_PE_BUSY  0x08u   /* the bus was busy: nothing was done */

typedef struct mp_poll {
    mp_poll_cfg c;
    uint32_t last_active;  /* clock of the last RX/TX event */
    uint16_t cur;          /* current interval, ms */
    uint8_t active;        /* 1 once any activity was seen */
    uint8_t pad;
    uint32_t polls, bursts, immediates, busies;
} mp_poll;

void mp_poll_defaults(mp_poll_cfg *c);
/* cfg NULL = defaults. Out-of-range values are clamped (burst >= 1,
 * idle >= burst). Starts idle at idle_ms. */
void mp_poll_init(mp_poll *p, const mp_poll_cfg *cfg, uint32_t now);
/* After a poll at 'now' with 'events': ms until the next poll (0 = poll again
 * now; the caller bounds how many immediate polls it runs per timer firing). */
uint16_t mp_poll_after(mp_poll *p, uint32_t now, uint8_t events);
/* Activity outside the poll task (EWrite sent or queued a frame): switch to
 * burst. Returns the burst interval so the glue can re-prime a timer that is
 * further away. */
uint16_t mp_poll_kick(mp_poll *p, uint32_t now);

#endif

/* mp_poll.c: adaptive poll interval. See mp_poll.h. */
#include <string.h>
#include "mp_poll.h"

void mp_poll_defaults(mp_poll_cfg *c)
{
    c->burst_ms = MP_POLL_BURST_MS;
    c->hold_ms = MP_POLL_HOLD_MS;
    c->idle_ms = MP_POLL_IDLE_MS;
    c->step_ms = MP_POLL_STEP_MS;
    c->busy_ms = MP_POLL_BUSY_MS;
    c->mid_ms = 0;
    c->quiet_ms = 0;
}

void mp_poll_init(mp_poll *p, const mp_poll_cfg *cfg, uint32_t now)
{
    memset(p, 0, sizeof(*p));
    if (cfg)
        p->c = *cfg;
    else
        mp_poll_defaults(&p->c);
    if (p->c.burst_ms == 0)
        p->c.burst_ms = 1;
    if (p->c.idle_ms < p->c.burst_ms)
        p->c.idle_ms = p->c.burst_ms;
    if (p->c.busy_ms == 0)
        p->c.busy_ms = 1;
    if (p->c.mid_ms) {
        if (p->c.mid_ms < p->c.burst_ms)
            p->c.mid_ms = p->c.burst_ms;
        if (p->c.mid_ms > p->c.idle_ms)
            p->c.mid_ms = p->c.idle_ms;
        if (p->c.quiet_ms < p->c.hold_ms)
            p->c.quiet_ms = p->c.hold_ms;
    }
    p->cur = p->c.idle_ms;
    p->last_active = now;
}

static uint16_t go_burst(mp_poll *p, uint32_t now)
{
    p->last_active = now;
    p->active = 1;
    p->cur = p->c.burst_ms;
    p->bursts++;
    return p->cur;
}

uint16_t mp_poll_after(mp_poll *p, uint32_t now, uint8_t events)
{
    uint32_t next;

    p->polls++;
    if (events & MP_PE_BUSY) {
        p->busies++;
        return p->c.busy_ms;
    }
    if (events & (MP_PE_RX | MP_PE_TX))
        (void)go_burst(p, now);
    if (events & MP_PE_MORE) {
        p->immediates++;
        return 0;
    }
    if (events & (MP_PE_RX | MP_PE_TX))
        return p->cur;

    /* Idle poll. */
    if (p->active && mp_time_since(now, p->last_active) < p->c.hold_ms)
        return p->cur;
    if (p->c.mid_ms && p->active && mp_time_since(now, p->last_active) < p->c.quiet_ms) {
        p->cur = p->c.mid_ms;
        return p->cur;
    }
    if (p->c.mid_ms && p->cur < p->c.mid_ms)
        p->cur = p->c.mid_ms;
    next = p->c.step_ms ? (uint32_t)p->cur + p->c.step_ms : (uint32_t)p->cur * 2u;
    if (next > p->c.idle_ms)
        next = p->c.idle_ms;
    p->cur = (uint16_t)next;
    return p->cur;
}

uint16_t mp_poll_kick(mp_poll *p, uint32_t now)
{
    return go_burst(p, now);
}

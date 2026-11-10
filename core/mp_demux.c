/* mp_demux.c: protocol handler table. See mp_demux.h. */
#include <string.h>
#include "mp_demux.h"

void mp_demux_init(mp_demux *d)
{
    memset(d, 0, sizeof(*d));
}

mp_ph *mp_demux_find(mp_demux *d, uint16_t type)
{
    uint16_t i;
    for (i = 0; i < MP_DEMUX_MAX; i++)
        if (d->e[i].used && d->e[i].type == type)
            return &d->e[i];
    return NULL;
}

int mp_demux_attach(mp_demux *d, uint16_t type, void *handler)
{
    uint16_t i;
    if (mp_demux_find(d, type))
        return MP_LAP_PROT_ERR;
    for (i = 0; i < MP_DEMUX_MAX; i++) {
        mp_ph *e = &d->e[i];
        if (!e->used) {
            e->type = type;
            e->handler = handler;
            e->frames = 0;
            MP_BARRIER();
            e->used = 1;
            return MP_OK;
        }
    }
    return MP_LAP_PROT_ERR;
}

int mp_demux_detach(mp_demux *d, uint16_t type)
{
    mp_ph *e = mp_demux_find(d, type);
    if (!e)
        return MP_LAP_PROT_ERR;
    e->used = 0;
    MP_BARRIER();
    return MP_OK;
}

uint16_t mp_demux_type(const uint8_t *frame)
{
    uint16_t t = mp_rd16(frame + 12);
    return t <= MP_ETYPE_MAXLEN ? (uint16_t)MP_ETYPE_8023 : t;
}

mp_ph *mp_demux_lookup(mp_demux *d, const uint8_t *frame, uint16_t len)
{
    mp_ph *e;
    if (len < 14) {
        d->runts++;
        return NULL;
    }
    e = mp_demux_find(d, mp_demux_type(frame));
    if (!e) {
        d->unattached++;
        return NULL;
    }
    e->frames++;
    return e;
}

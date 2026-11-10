/* mp_ring.c: fixed-slot SPSC frame ring. See mp_ring.h. */
#include "mp_ring.h"

static uint16_t ring_slot(const mp_ring *r, uint16_t idx)
{
    return idx >= r->count ? (uint16_t)(idx - r->count) : idx;
}

static uint16_t ring_adv(const mp_ring *r, uint16_t idx, uint16_t n)
{
    uint16_t two = (uint16_t)(r->count * 2u);
    idx = (uint16_t)(idx + n);
    return idx >= two ? (uint16_t)(idx - two) : idx;
}

static uint8_t *slot_ptr(const mp_ring *r, uint16_t idx)
{
    /* 16x16->32 multiply: one MULU on the 68000. */
    return r->mem + (uint32_t)ring_slot(r, idx) * MP_SLOT_SIZE;
}

int mp_ring_init(mp_ring *r, uint8_t *mem, uint16_t count)
{
    if (!r || !mem || count == 0 || count > MP_RING_MAX)
        return MP_E_ARG;
    r->mem = mem;
    r->count = count;
    r->head = 0;
    r->tail = 0;
    return MP_OK;
}

void mp_ring_reset(mp_ring *r)
{
    r->head = 0;
    r->tail = 0;
}

uint16_t mp_ring_used(const mp_ring *r)
{
    uint16_t h = r->head, t = r->tail;
    return h >= t ? (uint16_t)(h - t) : (uint16_t)(h + r->count * 2u - t);
}

uint16_t mp_ring_free(const mp_ring *r)
{
    return (uint16_t)(r->count - mp_ring_used(r));
}

uint8_t *mp_ring_claim(mp_ring *r)
{
    if (mp_ring_used(r) >= r->count)
        return NULL;
    return slot_ptr(r, r->head) + MP_SLOT_HDR;
}

void mp_ring_commit(mp_ring *r, uint16_t len)
{
    uint8_t *s = slot_ptr(r, r->head);
    if (len > MP_SLOT_DATA)
        len = MP_SLOT_DATA;
    mp_wr16(s, len);
    s[2] = 0;
    s[3] = 0;
    MP_BARRIER();
    r->head = ring_adv(r, r->head, 1);
}

const uint8_t *mp_ring_peek_at(const mp_ring *r, uint16_t i, uint16_t *len)
{
    const uint8_t *s;
    if (i >= mp_ring_used(r))
        return NULL;
    MP_BARRIER();
    s = slot_ptr(r, ring_adv(r, r->tail, i));
    if (len)
        *len = mp_rd16(s);
    return s + MP_SLOT_HDR;
}

void mp_ring_pop_n(mp_ring *r, uint16_t n)
{
    uint16_t used = mp_ring_used(r);
    if (n > used)
        n = used;
    MP_BARRIER();
    r->tail = ring_adv(r, r->tail, n);
}

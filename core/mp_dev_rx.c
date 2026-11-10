/* mp_dev_rx.c: incremental DaynaPORT READ(6) response parser.
 *
 * Wire format [fw network.c case 0x08; snow ethernet.rs 0x08]:
 *   empty queue:  6 zero bytes, then STATUS
 *   each packet:  len_hi len_lo 0 0 0 flags, then len bytes; len counts the
 *                 frame plus 4 CRC bytes (frame padded to 60 first on new fw,
 *                 len padded to 64 on old fw, frame padded to 64 on Snow);
 *                 flags 0x10 = another packet is queued.
 *   multi-packet: new fw with CDB[5] = 0xC0 and Snow (always) send further
 *                 packets in the same DATA IN phase. Old fw sends one per READ.
 * The parser does not trust flags to mean "in this READ": the glue checks the
 * bus phase after every packet (PLAN 2.2).
 */
#include <string.h>
#include "mp_dev.h"

void mp_rx_init(mp_rx *p, mp_ring *ring)
{
    memset(p, 0, sizeof(*p));
    p->ring = ring;
    p->state = MP_RX_IDLE;
}

void mp_rx_set_full(mp_rx *p, void (*fn)(void *ctx), void *ctx)
{
    p->on_full = fn;
    p->full_ctx = ctx;
}

static void want_hdr(mp_rx *p)
{
    p->state = MP_RX_HDR;
    p->dst = p->hdr;
    p->need = MP_DEV_RX_HDR;
}

void mp_rx_begin(mp_rx *p)
{
    p->npkt = 0;
    p->flags = 0;
    p->st.polls++;
    want_hdr(p);
}

uint8_t mp_rx_want(const mp_rx *p, uint8_t **dst, uint16_t *n)
{
    if (p->state == MP_RX_HDR || p->state == MP_RX_DATA || p->state == MP_RX_SKIP) {
        if (dst)
            *dst = p->dst;
        if (n)
            *n = p->need;
    } else {
        if (n)
            *n = 0;
    }
    return p->state;
}

static void skip(mp_rx *p, uint32_t *ctr)
{
    (*ctr)++;
    p->state = MP_RX_SKIP;
    p->dst = p->sink;
    p->need = p->len;
}

static void header_done(mp_rx *p)
{
    uint8_t *slot;
    p->len = mp_rd16(p->hdr);
    p->flags = p->hdr[5];
    if (p->len == 0) {
        /* Empty queue. A zero len after a packet is not sent by any known
         * device; treat it the same way (nothing follows). */
        if (p->npkt == 0)
            p->st.empty++;
        p->state = MP_RX_EMPTY;
        return;
    }
    if (p->npkt < 255u)
        p->npkt++;
    if (p->npkt == 2) {
        p->st.multi++;
        p->saw_multi = 1;
    }
    if (p->len < MP_ETH_HDR + MP_DEV_CRC) {
        skip(p, &p->st.bad);
        return;
    }
    if (p->len > MP_DEV_PKT_MAX) {
        skip(p, &p->st.oversize);
        return;
    }
    slot = p->ring ? mp_ring_claim(p->ring) : NULL;
    if (!slot && p->ring && p->on_full) {
        p->on_full(p->full_ctx);
        slot = mp_ring_claim(p->ring);
        if (slot)
            p->st.drained++;
    }
    if (!slot) {
        skip(p, &p->st.overflow);
        return;
    }
    p->state = MP_RX_DATA;
    p->dst = slot;
    p->need = p->len;
}

uint8_t mp_rx_got(mp_rx *p, uint16_t n)
{
    if (p->state != MP_RX_HDR && p->state != MP_RX_DATA && p->state != MP_RX_SKIP)
        return p->state;
    if (n > p->need)
        n = p->need;
    p->need = (uint16_t)(p->need - n);
    if (p->state != MP_RX_SKIP)
        p->dst += n;
    if (p->need)
        return p->state;
    switch (p->state) {
    case MP_RX_HDR:
        header_done(p);
        break;
    case MP_RX_DATA:
        /* Keep the CRC bytes in the slot but not in the frame length. */
        mp_ring_commit(p->ring, (uint16_t)(p->len - MP_DEV_CRC));
        p->st.frames++;
        p->state = MP_RX_PKT;
        break;
    default:
        p->state = MP_RX_PKT;
        break;
    }
    return p->state;
}

void mp_rx_next(mp_rx *p)
{
    if (p->state == MP_RX_PKT)
        want_hdr(p);
}

int mp_rx_end(mp_rx *p)
{
    int more = (p->state == MP_RX_PKT) && (p->flags & MP_DEV_RX_MORE) != 0;
    if (more)
        p->st.more_ended++;
    p->state = MP_RX_IDLE;
    return more;
}

void mp_rx_abort(mp_rx *p)
{
    /* A claimed but uncommitted slot is simply claimed again next time. */
    p->st.aborted++;
    p->state = MP_RX_IDLE;
}

int mp_rx_poll(mp_rx *p, const mp_bus *bus, uint8_t mode, uint16_t alloc,
               uint16_t lead, int *more)
{
    uint8_t cdb[6];
    uint8_t *dst;
    uint16_t n;
    uint32_t frames0;
    int rc, status, m;

    if (more)
        *more = 0;
    mp_cdb_read(cdb, alloc, mode);
    rc = bus->begin(bus->ctx, cdb);
    if (rc != 0)
        return rc == MP_E_BUSY ? MP_E_BUSY : MP_E_BUS;

    mp_rx_begin(p);
    frames0 = p->st.frames;
    for (;;) {
        uint8_t s = mp_rx_want(p, &dst, &n);
        if (s == MP_RX_HDR) {
            if (bus->xin(bus->ctx, dst, n, MP_XF_POLLED) != 0)
                goto bus_err;
            if (mp_rx_got(p, n) == MP_RX_DATA && lead) {
                /* First payload bytes polled too, so the device's 75 us pause
                 * after the header falls inside handshaked bytes [fw]. */
                mp_rx_want(p, &dst, &n);
                if (n > lead)
                    n = lead;
                if (bus->xin(bus->ctx, dst, n, MP_XF_POLLED) != 0)
                    goto bus_err;
                mp_rx_got(p, n);
            }
        } else if (s == MP_RX_DATA) {
            if (bus->xin(bus->ctx, dst, n, MP_XF_BLIND) != 0)
                goto bus_err;
            mp_rx_got(p, n);
        } else if (s == MP_RX_SKIP) {
            if (bus->xin(bus->ctx, dst, n, MP_XF_NOINC) != 0)
                goto bus_err;
            mp_rx_got(p, n);
        } else if (s == MP_RX_PKT) {
            /* Phase decides, not flags: DATA IN = another header follows. */
            if (p->npkt < 255u && bus->in_data(bus->ctx))
                mp_rx_next(p);
            else
                break;
        } else {
            break;   /* MP_RX_EMPTY */
        }
    }
    m = mp_rx_end(p);
    status = bus->end(bus->ctx);
    if (status != 0)
        return status < 0 ? MP_E_BUS : MP_E_STATUS;
    if (more)
        *more = m;
    return (int)(p->st.frames - frames0);

bus_err:
    mp_rx_abort(p);
    (void)bus->end(bus->ctx);
    return MP_E_BUS;
}

/* mp_dev_tx.c: DaynaPORT WRITE(6): universal single-frame and batched preamble writes.
 *
 * [fw network.c case 0x0a] with CDB[5] != 0: loop { read 4-byte header; len 0
 * -> done; read len bytes; send }. [fw before e7d9629] with CDB[5] == 0x80:
 * read CDB[3..4] + 8 bytes, take len from the header, send bytes 4..4+len.
 * [snow ethernet.rs 0x0A] with CDB[5] bit 7: header, then (packet + next
 * header) until a zero length. So [len 0 0][frame][00 00 00 00] with
 * CDB[3..4] = len works on all three; more than one frame per WRITE needs the
 * new firmware (or Snow).
 */
#include <string.h>
#include "mp_dev.h"

static const uint8_t mp_zeros[MP_ETH_MIN] = { 0 };

void mp_tx_init(mp_tx *t, mp_ring *q, const uint8_t mac[6], int batch)
{
    uint16_t i;
    memset(t, 0, sizeof(*t));
    t->q = q;
    for (i = 0; i < 6; i++)
        t->mac[i] = mac ? mac[i] : 0;
    t->batch = batch ? 1 : 0;
    t->max_batch = MP_TX_BATCH_MAX;
}

uint32_t mp_seg_total(const mp_seg *s, uint16_t n)
{
    uint32_t total = 0;
    uint16_t i;
    for (i = 0; i < n; i++)
        total += s[i].len;
    return total;
}

static int len_ok(uint32_t total)
{
    return total >= MP_ETH_HDR && total <= MP_ETH_MAX;
}

int mp_tx_queue(mp_tx *t, const mp_seg *s, uint16_t n)
{
    uint32_t total = mp_seg_total(s, n);
    uint8_t *slot, *w;
    uint16_t i, plen;

    if (!len_ok(total)) {
        t->st.badlen++;
        return MP_E_LEN;
    }
    slot = mp_ring_claim(t->q);
    if (!slot) {
        t->st.full++;
        return MP_E_FULL;
    }
    w = slot;
    for (i = 0; i < n; i++) {
        if (s[i].len) {
            memcpy(w, s[i].p, s[i].len);
            w += s[i].len;
        }
    }
    memcpy(slot + 6, t->mac, 6);
    plen = mp_dev_tx_padlen((uint16_t)total);
    if (plen > total)
        memset(slot + total, 0, plen - (uint16_t)total);
    mp_ring_commit(t->q, plen);
    t->st.queued++;
    return MP_OK;
}

static int finish(const mp_bus *bus, int rc)
{
    int status = bus->end(bus->ctx);
    if (rc != 0)
        return MP_E_BUS;
    if (status != 0)
        return status < 0 ? MP_E_BUS : MP_E_STATUS;
    return MP_OK;
}

int mp_tx_send(mp_tx *t, const mp_bus *bus, mp_seg *s, uint16_t n)
{
    uint8_t cdb[6], hdr[4];
    uint32_t total = mp_seg_total(s, n);
    uint16_t i, plen;
    int rc;

    if (!len_ok(total) || n == 0 || s[0].len < 12) {
        t->st.badlen++;
        return MP_E_LEN;
    }
    memcpy(s[0].p + 6, t->mac, 6);
    plen = mp_dev_tx_padlen((uint16_t)total);
    mp_cdb_write_preamble(cdb, plen);
    rc = bus->begin(bus->ctx, cdb);
    if (rc != 0)
        return rc == MP_E_BUSY ? MP_E_BUSY : MP_E_BUS;
    t->st.writes++;
    mp_dev_tx_hdr(hdr, plen);
    rc = bus->xout(bus->ctx, hdr, 4, MP_XF_POLLED);
    for (i = 0; rc == 0 && i < n; i++)
        if (s[i].len)
            rc = bus->xout(bus->ctx, s[i].p, s[i].len, MP_XF_BLIND);
    if (rc == 0 && plen > total)
        rc = bus->xout(bus->ctx, mp_zeros, (uint16_t)(plen - total), MP_XF_POLLED);
    if (rc == 0)
        rc = bus->xout(bus->ctx, mp_zeros, 4, MP_XF_POLLED);
    rc = finish(bus, rc);
    if (rc == MP_OK)
        t->st.sent++;
    else
        t->st.errors++;
    return rc;
}

int mp_tx_flush(mp_tx *t, const mp_bus *bus)
{
    uint8_t cdb[6], hdr[4];
    const uint8_t *f;
    uint16_t used, k, i, len, accepted = 0;
    int rc;

    used = mp_ring_used(t->q);
    if (used == 0)
        return 0;
    k = 1;
    if (t->batch) {
        k = used;
        if (k > t->max_batch)
            k = t->max_batch;
        if (k > MP_TX_BATCH_MAX)
            k = MP_TX_BATCH_MAX;
        if (k == 0)
            k = 1;
    }
    f = mp_ring_peek(t->q, &len);
    mp_cdb_write_preamble(cdb, len);
    rc = bus->begin(bus->ctx, cdb);
    if (rc != 0)
        return rc == MP_E_BUSY ? MP_E_BUSY : MP_E_BUS;
    t->st.writes++;
    rc = 0;
    for (i = 0; rc == 0 && i < k; i++) {
        f = mp_ring_peek_at(t->q, i, &len);
        mp_dev_tx_hdr(hdr, len);
        rc = bus->xout(bus->ctx, hdr, 4, MP_XF_POLLED);
        if (rc == 0)
            rc = bus->xout(bus->ctx, f, len, MP_XF_BLIND);
        if (rc == 0)
            accepted = (uint16_t)(i + 1u);
    }
    if (rc == 0)
        rc = bus->xout(bus->ctx, mp_zeros, 4, MP_XF_POLLED);
    rc = finish(bus, rc);

    if (rc == MP_OK) {
        mp_ring_pop_n(t->q, k);
        t->st.sent += k;
        if (k >= 2)
            t->st.batches++;
        return (int)k;
    }
    /* Frames whose bytes all went out were taken by the device (old firmware
     * reads len + 8 and stops, so in a batch the first frame is sent and the
     * second fails in its data phase). Keep the rest, stop batching. */
    t->st.errors++;
    if (accepted) {
        mp_ring_pop_n(t->q, accepted);
        t->st.sent += accepted;
    }
    if (k >= 2 && t->batch && accepted < k) {
        t->batch = 0;
        t->st.batch_off++;
    }
    return rc;
}

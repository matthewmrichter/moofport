/* fakedev.c: host model of the BlueSCSI DaynaPORT target. See fakedev.h. */
#include <string.h>
#include "fakedev.h"

#define DAYNAPORT_SCSI_PACKET_MAX 1524u   /* network.h */

enum { W_NONE = 0, W_RAW, W_NEW_HDR, W_NEW_DATA, W_OLD_CONT, W_SNOW_HDR, W_SNOW_PKT,
       W_MCAST, W_JOIN };

uint32_t fd_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

void fd_init(fakedev *d, int kind)
{
    memset(d, 0, sizeof(*d));
    d->kind = kind;
    d->mac[0] = 0x00; d->mac[1] = 0x80; d->mac[2] = 0x19;   /* Dayna OUI, as Snow uses */
    d->mac[3] = 0x12; d->mac[4] = 0x34; d->mac[5] = 0x56;
    d->phase = FD_FREE;
}

uint16_t fd_queued(const fakedev *d)
{
    if (d->kind == FD_SNOW)
        return d->qn;
    return d->wr >= d->rd ? (uint16_t)(d->wr - d->rd)
                          : (uint16_t)(d->wr + FD_FW_QUEUE - d->rd);
}

int fd_net_rx(fakedev *d, const uint8_t *frame, uint16_t len)
{
    uint8_t *slot;
    uint32_t crc;

    if (d->kind == FD_SNOW) {
        /* Snow queues raw packets; padding and FCS are added at READ time. */
        if (d->qn >= FD_QCAP || len > FD_PKT) {
            d->net_dropped++;
            return 0;
        }
        slot = d->q[(d->rd + d->qn) % FD_QCAP];
        memcpy(slot, frame, len);
        d->qlen[(d->rd + d->qn) % FD_QCAP] = len;
        d->qn++;
        return 1;
    }

    /* scsiNetworkEnqueue */
    if (!d->enabled) {
        d->net_dropped++;
        return 0;
    }
    if ((uint32_t)len + 4u > FD_PKT) {
        d->net_dropped++;
        return 0;
    }
    slot = d->q[d->wr];
    memcpy(slot, frame, len);
    if (d->kind == FD_FW_2608 && len < 60) {
        memset(slot + len, 0, 60u - len);
        len = 60;
    }
    crc = fd_crc32(slot, len);
    slot[len] = (uint8_t)crc;
    slot[len + 1] = (uint8_t)(crc >> 8);
    slot[len + 2] = (uint8_t)(crc >> 16);
    slot[len + 3] = (uint8_t)(crc >> 24);
    d->qlen[d->wr] = (uint16_t)(len + 4u);
    d->wr = (uint16_t)(d->wr == FD_FW_QUEUE - 1u ? 0 : d->wr + 1u);
    /* write index caught up to read index: the queue now reads as empty */
    return 1;
}

static void in_put(fakedev *d, const uint8_t *p, uint32_t n)
{
    memcpy(d->in + d->in_len, p, n);
    d->in_len += n;
}

static void gap(fakedev *d)
{
    if (d->ngaps < FD_GAPS_MAX)
        d->gaps[d->ngaps++] = d->in_len;
}

static void fw_adv(fakedev *d)
{
    d->rd = (uint16_t)(d->rd == FD_FW_QUEUE - 1u ? 0 : d->rd + 1u);
}

static void read_empty(fakedev *d)
{
    static const uint8_t z[6] = { 0 };
    in_put(d, z, 6);
}

static int do_read(fakedev *d, uint16_t size)
{
    uint8_t h[6];
    if (size == 1)
        return 0;   /* CHECK CONDITION [fw, snow] */

    if (d->kind == FD_FW_OLD) {
        uint32_t psize;
        uint16_t idx;
        if (d->rd == d->wr) {
            read_empty(d);
            return 1;
        }
        idx = d->rd;
        psize = d->qlen[idx];
        if (psize < 64) {
            memset(d->q[idx] + psize, 0, 64u - psize);   /* stale buffer bytes; zero here */
            psize = 64;
        } else if (psize + 6u > size) {
            psize = size - 6u;
        }
        fw_adv(d);
        h[0] = (uint8_t)(psize >> 8); h[1] = (uint8_t)psize;
        h[2] = h[3] = h[4] = 0;
        h[5] = (uint8_t)(d->rd == d->wr ? 0 : 0x10);
        in_put(d, h, 6);
        gap(d);    /* s2s_delay_us(80) */
        in_put(d, d->q[idx], psize);
        return 1;
    }

    if (d->kind == FD_FW_2608) {
        int multi = (d->cdb[5] & 0x40) != 0;
        int done;
        uint32_t total;
        if (d->rd == d->wr) {
            read_empty(d);
            return 1;
        }
        for (done = 0, total = 0; !done;) {
            uint16_t idx = d->rd;
            uint32_t len = d->qlen[idx];
            total += len + 6u;
            fw_adv(d);
            done = (d->rd == d->wr);
            if (!done && !multi)
                done = 1;
            if (!done && total + DAYNAPORT_SCSI_PACKET_MAX + 6u > size)
                done = 1;
            if (!done && total >= (DAYNAPORT_SCSI_PACKET_MAX + 6u) * 2u)
                done = 1;
            h[0] = (uint8_t)(len >> 8); h[1] = (uint8_t)len;
            h[2] = h[3] = h[4] = 0;
            h[5] = (uint8_t)(done ? 0 : 0x10);
            in_put(d, h, 6);
            gap(d);                     /* sleep_us(75) */
            in_put(d, d->q[idx], len);
            if (!done)
                gap(d);                 /* sleep_us(300) */
        }
        return 1;
    }

    /* FD_SNOW */
    {
        unsigned count = 0;
        if (d->qn == 0) {
            read_empty(d);
            return 1;
        }
        for (;;) {
            uint16_t idx = d->rd;
            uint32_t plen = d->qlen[idx], padded = plen < 64 ? 64 : plen;
            uint32_t flen = padded + 4u;
            uint32_t crc;
            int more;
            uint8_t c[4];
            d->rd = (uint16_t)((d->rd + 1u) % FD_QCAP);
            d->qn--;
            count++;
            more = d->qn != 0 && count < 8;
            if (size < 6u + flen) {
                d->in_len = 0;          /* whole response dropped */
                d->ngaps = 0;
                return 0;
            }
            crc = fd_crc32(d->q[idx], plen);
            h[0] = (uint8_t)(flen >> 8); h[1] = (uint8_t)flen;
            h[2] = h[3] = h[4] = 0;
            h[5] = (uint8_t)(more ? 0x10 : 0);
            in_put(d, h, 6);
            in_put(d, d->q[idx], plen);
            if (padded > plen) {
                memset(d->in + d->in_len, 0, padded - plen);
                d->in_len += padded - plen;
            }
            c[0] = (uint8_t)(crc >> 24); c[1] = (uint8_t)(crc >> 16);
            c[2] = (uint8_t)(crc >> 8); c[3] = (uint8_t)crc;
            in_put(d, c, 4);
            if (!more)
                break;
        }
        return 1;
    }
}

static void sent_frame(fakedev *d, const uint8_t *p, uint32_t n)
{
    if (d->nsent < FD_SENT_MAX) {
        memcpy(d->sent[d->nsent], p, n > FD_PKT ? FD_PKT : n);
        d->sent_len[d->nsent] = (uint16_t)n;
        d->nsent++;
    }
}

static void wifi_reply(fakedev *d, uint8_t sub, uint16_t size)
{
    uint8_t *o = d->in;
    d->in_len = 0;
    switch (sub) {
    case MP_WIFI_SCAN:
    case MP_WIFI_COMPLETE:
        o[0] = 1;
        d->in_len = 1;
        break;
    case MP_WIFI_RESULTS: {
        /* two canned networks */
        uint16_t i, nets = 2, netsize = (uint16_t)(nets * MP_WIFI_ENTRY_LEN);
        if (size < 2) {
            d->phase = FD_STATUS;
            d->status = 2;
            return;
        }
        if (netsize + 2u > size)
            netsize = (uint16_t)(((size - 2u) / MP_WIFI_ENTRY_LEN) * MP_WIFI_ENTRY_LEN);
        memset(o, 0, 2u + nets * MP_WIFI_ENTRY_LEN);
        o[0] = (uint8_t)(netsize >> 8); o[1] = (uint8_t)netsize;
        for (i = 0; i < nets; i++) {
            uint8_t *e = o + 2 + i * MP_WIFI_ENTRY_LEN;
            memcpy(e, i ? "Other" : "MoofNet", i ? 5 : 7);
            e[64] = 0xAA; e[69] = (uint8_t)i;
            e[70] = (uint8_t)(i ? -70 : -42);
            e[71] = (uint8_t)(i ? 11 : 6);
            e[72] = (uint8_t)(i ? 0 : 1);
        }
        d->in_len = netsize + 2u;
        break;
    }
    case MP_WIFI_INFO: {
        uint8_t *e = o + 2;
        memset(o, 0, 2u + MP_WIFI_ENTRY_LEN);
        o[0] = 0; o[1] = MP_WIFI_ENTRY_LEN;
        memcpy(e, d->wifi_ssid, strlen(d->wifi_ssid));
        e[70] = (uint8_t)-50;
        e[71] = 6;
        e[72] = 1;
        d->in_len = 2u + MP_WIFI_ENTRY_LEN;
        break;
    }
    case MP_WIFI_JOIN:
        if (size != MP_WIFI_JOIN_LEN) {
            d->phase = FD_STATUS;
            d->status = 2;
            return;
        }
        d->wstage = W_JOIN;
        d->wwant = MP_WIFI_JOIN_LEN;
        d->wgot = 0;
        d->phase = FD_DATA_OUT;
        return;
    default:
        d->phase = FD_STATUS;
        return;
    }
    d->phase = FD_DATA_IN;
}

static int fd_begin(void *ctx, const uint8_t *cdb)
{
    fakedev *d = (fakedev *)ctx;
    uint16_t size;
    if (d->busy_next > 0) {
        d->busy_next--;
        return MP_E_BUSY;
    }
    if (d->phase != FD_FREE)
        return MP_E_BUSY;   /* a transaction is open: SCSIGet says scMgrBusyErr */
    memcpy(d->cdb, cdb, 6);
    size = (uint16_t)((cdb[3] << 8) | cdb[4]);
    d->cmds++;
    d->status = 0;
    d->in_len = d->in_pos = 0;
    d->ngaps = 0;
    d->wstage = W_NONE;
    d->wgot = 0;
    d->wsize = size;
    d->phase = FD_STATUS;

    switch (cdb[0]) {
    case MP_OP_READ:
        d->reads++;
        if (do_read(d, size))
            d->phase = FD_DATA_IN;
        else
            d->status = 2;
        break;
    case MP_OP_WRITE:
        d->writes++;
        d->phase = FD_DATA_OUT;
        if (d->kind == FD_FW_OLD) {
            if (cdb[5] == 0x80) {
                d->wstage = W_OLD_CONT;
                d->wwant = (uint32_t)size + 8u;
            } else {
                d->wstage = W_RAW;
                d->wwant = size;
            }
        } else if (d->kind == FD_FW_2608) {
            if (cdb[5] == 0) {
                d->wstage = W_RAW;
                d->wwant = size > FD_PKT ? FD_PKT : size;
            } else {
                d->wstage = W_NEW_HDR;
                d->wwant = 4;
            }
        } else {
            if (cdb[5] & 0x80) {
                d->wstage = W_SNOW_HDR;
                d->wwant = 4;
            } else {
                d->wstage = W_RAW;
                d->wwant = size;
            }
        }
        if (d->wwant > sizeof(d->wbuf))
            d->wwant = sizeof(d->wbuf);
        break;
    case MP_OP_STATS:
        memset(d->in, 0, 18);
        memcpy(d->in, d->mac, 6);
        d->in_len = 18;
        d->phase = FD_DATA_IN;
        break;
    case MP_OP_MCAST:
        d->wstage = W_MCAST;
        d->wwant = d->kind == FD_SNOW ? 6u : size;
        d->phase = FD_DATA_OUT;
        break;
    case MP_OP_ENABLE:
        if (cdb[5] & 0x80) {
            if (d->kind == FD_SNOW ? !d->enabled : 1) {
                d->rd = d->wr = 0;
                d->qn = 0;
            }
            d->enabled = 1;
        } else {
            d->enabled = 0;
        }
        break;
    case MP_OP_INQUIRY: {
        uint8_t *r = d->in;
        memset(r, 0, 36);
        r[0] = 3; r[2] = 1; r[3] = 2; r[4] = 31; r[7] = 0x18;
        memcpy(r + 8, "Dayna   ", 8);
        memcpy(r + 16, "SCSI/Link       ", 16);
        memcpy(r + 32, "2.0f", 4);
        d->in_len = cdb[4] < 36 ? cdb[4] : 36;
        d->phase = d->in_len ? FD_DATA_IN : FD_STATUS;
        break;
    }
    case MP_OP_WIFI:
        if (d->kind == FD_SNOW) {
            d->status = 2;   /* not implemented in Snow: CHECK CONDITION */
            break;
        }
        wifi_reply(d, cdb[1], size);
        break;
    default:
        if (d->kind == FD_SNOW)
            d->status = 2;
        break;
    }
    if (d->status)
        d->checks++;
    return 0;
}

static int crosses_gap(const fakedev *d, uint32_t start, uint32_t n, int *starts)
{
    uint16_t i;
    int cross = 0;
    *starts = 0;
    for (i = 0; i < d->ngaps; i++) {
        uint32_t g = d->gaps[i];
        if (g == start)
            *starts = 1;
        else if (g > start && g < start + n)
            cross = 1;
    }
    return cross;
}

static int fd_xin(void *ctx, uint8_t *dst, uint16_t n, uint8_t how)
{
    fakedev *d = (fakedev *)ctx;
    uint32_t avail, m, i;
    int starts;
    if (d->phase != FD_DATA_IN)
        return MP_E_BUS;
    avail = d->in_len - d->in_pos;
    m = n < avail ? n : avail;
    if (d->fail_in_after && d->in_pos + m > d->fail_in_after) {
        /* injected bus error: the transfer stops at the fail point */
        m = d->fail_in_after > d->in_pos ? d->fail_in_after - d->in_pos : 0;
        d->fail_in_after = 0;
        d->in_pos += m;
        d->phase = FD_STATUS;
        return MP_E_BUS;
    }
    if (how == MP_XF_BLIND) {
        if (crosses_gap(d, d->in_pos, m, &starts))
            d->blind_gap_cross++;
        if (starts)
            d->blind_gap_start++;
        d->xfer_blind += m;
    } else if (how == MP_XF_NOINC) {
        d->xfer_noinc += m;
    } else {
        d->xfer_polled += m;
    }
    if (how == MP_XF_NOINC) {
        for (i = 0; i < m; i++)
            dst[i % MP_RX_SINK] = d->in[d->in_pos + i];
    } else {
        memcpy(dst, d->in + d->in_pos, m);
    }
    d->in_pos += m;
    if (d->in_pos >= d->in_len)
        d->phase = FD_STATUS;
    return m == n ? 0 : MP_E_BUS;
}

/* One complete piece of DATA OUT arrived in wbuf. */
static void w_piece(fakedev *d)
{
    uint32_t len;
    switch (d->wstage) {
    case W_RAW:
        sent_frame(d, d->wbuf, d->wgot);
        d->phase = FD_STATUS;
        break;
    case W_OLD_CONT:
        len = ((uint32_t)d->wbuf[0] << 8) | d->wbuf[1];
        if (len > d->wsize)
            len = d->wsize;   /* model: never copy past what was read */
        sent_frame(d, d->wbuf + 4, len);
        d->phase = FD_STATUS;
        break;
    case W_NEW_HDR:
        len = ((uint32_t)d->wbuf[0] << 8) | d->wbuf[1];
        if (len == 0) {
            d->phase = FD_STATUS;
            break;
        }
        if (len > FD_PKT)
            len = FD_PKT;
        d->wstage = W_NEW_DATA;
        d->wwant = len;
        break;
    case W_NEW_DATA:
        sent_frame(d, d->wbuf, d->wgot);
        d->wstage = W_NEW_HDR;
        d->wwant = 4;
        break;
    case W_SNOW_HDR:
    case W_SNOW_PKT:
        if (d->wstage == W_SNOW_PKT) {
            sent_frame(d, d->wbuf, d->wgot - 4u);
            len = ((uint32_t)d->wbuf[d->wgot - 4u] << 8) | d->wbuf[d->wgot - 3u];
        } else {
            len = ((uint32_t)d->wbuf[0] << 8) | d->wbuf[1];
        }
        if (len == 0) {
            d->phase = FD_STATUS;
            break;
        }
        d->wstage = W_SNOW_PKT;
        d->wwant = len + 4u;
        if (d->wwant > sizeof(d->wbuf))
            d->wwant = sizeof(d->wbuf);
        break;
    case W_MCAST:
        if (d->nmcast < 8 && d->wgot >= 6)
            memcpy(d->mcast[d->nmcast++], d->wbuf, 6);
        d->phase = FD_STATUS;
        break;
    case W_JOIN:
        memcpy(d->wifi_ssid, d->wbuf, 64);
        d->wifi_ssid[64] = 0;
        d->wifi_joined = 1;
        d->phase = FD_STATUS;
        break;
    default:
        d->phase = FD_STATUS;
        break;
    }
    d->wgot = 0;
}

static int fd_xout(void *ctx, const uint8_t *src, uint16_t n, uint8_t how)
{
    fakedev *d = (fakedev *)ctx;
    uint32_t done = 0;
    if (how == MP_XF_BLIND)
        d->xfer_blind += n;
    else
        d->xfer_polled += n;
    while (done < n) {
        uint32_t m;
        if (d->phase != FD_DATA_OUT)
            return MP_E_BUS;   /* target left DATA OUT: phase mismatch */
        m = d->wwant - d->wgot;
        if (m > n - done)
            m = n - done;
        memcpy(d->wbuf + d->wgot, src + done, m);
        d->wgot += m;
        done += m;
        if (d->wgot == d->wwant)
            w_piece(d);
    }
    return 0;
}

static int fd_in_data(void *ctx)
{
    fakedev *d = (fakedev *)ctx;
    return d->phase == FD_DATA_IN || d->phase == FD_DATA_OUT;
}

static int fd_end(void *ctx)
{
    fakedev *d = (fakedev *)ctx;
    int st = d->status;
    if (d->phase == FD_DATA_IN || d->phase == FD_DATA_OUT) {
        d->abandoned++;
        d->phase = FD_FREE;
        return MP_E_BUS;
    }
    d->phase = FD_FREE;
    return st;
}

void fd_bus(fakedev *d, mp_bus *bus)
{
    bus->ctx = d;
    bus->begin = fd_begin;
    bus->xin = fd_xin;
    bus->xout = fd_xout;
    bus->in_data = fd_in_data;
    bus->end = fd_end;
}

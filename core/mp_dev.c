/* mp_dev.c: DaynaPORT CDB builders and field codecs. See mp_dev.h for sources. */
#include "mp_dev.h"

static void cdb6(uint8_t cdb[6], uint8_t op, uint8_t b1, uint16_t len, uint8_t b5)
{
    cdb[0] = op;
    cdb[1] = b1;
    cdb[2] = 0;
    mp_wr16(cdb + 3, len);
    cdb[5] = b5;
}

void mp_cdb_inquiry(uint8_t cdb[6], uint8_t alloc)
{
    /* INQUIRY allocation length is the single byte CDB[4]. */
    cdb6(cdb, MP_OP_INQUIRY, 0, alloc, 0);
}

void mp_cdb_read(uint8_t cdb[6], uint16_t alloc, uint8_t mode)
{
    cdb6(cdb, MP_OP_READ, 0, alloc, mode);
}

void mp_cdb_write_raw(uint8_t cdb[6], uint16_t len)
{
    cdb6(cdb, MP_OP_WRITE, 0, len, MP_WRITE_RAW);
}

void mp_cdb_write_preamble(uint8_t cdb[6], uint16_t first_len)
{
    cdb6(cdb, MP_OP_WRITE, 0, first_len, MP_WRITE_PREAMBLE);
}

void mp_cdb_stats(uint8_t cdb[6])
{
    cdb6(cdb, MP_OP_STATS, 0, MP_STATS_LEN, 0);
}

void mp_cdb_mcast(uint8_t cdb[6])
{
    /* [fw] reads CDB[3..4] bytes; [snow] expects 6. */
    cdb6(cdb, MP_OP_MCAST, 0, 6, 0);
}

void mp_cdb_enable(uint8_t cdb[6], int on)
{
    cdb6(cdb, MP_OP_ENABLE, 0, 0, on ? 0x80u : 0x00u);
}

void mp_cdb_wifi(uint8_t cdb[6], uint8_t sub, uint16_t len)
{
    cdb6(cdb, MP_OP_WIFI, sub, len, 0);
}

static int field_is(const uint8_t *f, uint16_t flen, const char *s)
{
    uint16_t i = 0;
    while (s[i]) {
        if (i >= flen || f[i] != (uint8_t)s[i])
            return 0;
        i++;
    }
    /* Rest of the field must be padding (space or NUL). */
    for (; i < flen; i++)
        if (f[i] != ' ' && f[i] != 0)
            return 0;
    return 1;
}

int mp_dev_inquiry_match(const uint8_t *inq, uint16_t n)
{
    if (!inq || n < 32)
        return 0;
    return field_is(inq + 8, 8, "Dayna") && field_is(inq + 16, 16, "SCSI/Link");
}

uint16_t mp_dev_read_alloc(uint16_t free_slots, int safe)
{
    uint32_t a;
    if (free_slots == 0)
        free_slots = 1;
    if (safe)
        a = (uint32_t)(free_slots - 1u) * (MP_DEV_RX_MIN + MP_DEV_RX_HDR) + MP_DEV_READ_UNIT;
    else
        a = (uint32_t)free_slots * MP_DEV_READ_UNIT;
    return a > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)a;
}

void mp_dev_tx_hdr(uint8_t out[4], uint16_t len)
{
    mp_wr16(out, len);
    out[2] = 0;
    out[3] = 0;
}

uint16_t mp_dev_tx_padlen(uint16_t len)
{
    return len < MP_ETH_MIN ? (uint16_t)MP_ETH_MIN : len;
}

int mp_dev_parse_stats(const uint8_t *r, uint16_t n, uint8_t mac[6], uint32_t ctr[3])
{
    uint16_t i;
    if (n < 6)
        return MP_E_LEN;
    for (i = 0; i < 6; i++)
        mac[i] = r[i];
    if (ctr) {
        for (i = 0; i < 3; i++)
            ctr[i] = (n >= 6u + 4u * (i + 1u)) ? mp_rd32(r + 6 + 4 * i) : 0;
    }
    return MP_OK;
}

void mp_dev_wifi_entry(const uint8_t *p, mp_wifi_entry *e)
{
    uint16_t i;
    /* struct wifi_network_entry { char ssid[64]; char bssid[6]; int8_t rssi;
     *   uint8_t channel; uint8_t flags; uint8_t _padding; } packed [fw network.h] */
    for (i = 0; i < MP_WIFI_SSID_MAX && p[i]; i++)
        e->ssid[i] = (char)p[i];
    e->ssid[i] = 0;
    for (i = 0; i < 6; i++)
        e->bssid[i] = p[64 + i];
    e->rssi = (int8_t)(p[70] >= 0x80u ? (int)p[70] - 256 : (int)p[70]);
    e->channel = p[71];
    e->flags = p[72];
}

uint16_t mp_dev_wifi_list(const uint8_t *r, uint16_t n, const uint8_t **first)
{
    uint16_t len, cnt;
    if (first)
        *first = r + 2;
    if (n < 2)
        return 0;
    len = mp_rd16(r);
    if (len > n - 2u)
        len = (uint16_t)(n - 2u);
    cnt = 0;
    while (len >= MP_WIFI_ENTRY_LEN) {   /* no division: at most 10 entries [fw] */
        len = (uint16_t)(len - MP_WIFI_ENTRY_LEN);
        cnt++;
    }
    return cnt;
}

static void put_str(uint8_t *dst, uint16_t max, const char *s)
{
    uint16_t i;
    for (i = 0; i < max; i++)
        dst[i] = 0;
    for (i = 0; s && s[i] && i < max - 1u; i++)
        dst[i] = (uint8_t)s[i];
}

void mp_dev_wifi_join(uint8_t out[MP_WIFI_JOIN_LEN], const char *ssid,
                      const char *key, uint8_t channel)
{
    /* struct wifi_join_request { char ssid[64]; char key[64]; uint8_t channel;
     *   uint8_t _padding; } packed [fw network.h] */
    put_str(out, MP_WIFI_SSID_MAX, ssid);
    put_str(out + 64, MP_WIFI_KEY_MAX, key);
    out[128] = channel;
    out[129] = 0;
}

/* test_dev.c: CDB builders and field codecs (expected bytes from [fw] network.c
 * scsiNetworkCommand and [snow] ethernet.rs specific_cmd). */
#include "t.h"
#include "mp_dev.h"

static void test_cdb_read(void)
{
    uint8_t c[6];
    static const uint8_t single[6] = { 0x08, 0, 0, 0x05, 0xFA, 0x80 };
    static const uint8_t multi[6] = { 0x08, 0, 0, 0x0B, 0xF4, 0xC0 };
    mp_cdb_read(c, 1530, MP_READ_SINGLE);
    T_MEM(c, single, 6);
    mp_cdb_read(c, 3060, MP_READ_MULTI);
    T_MEM(c, multi, 6);
    T_CHECK((MP_READ_MULTI & 0x40) && !(MP_READ_SINGLE & 0x40));   /* bit 6 gate [fw 7c20579] */
}

static void test_cdb_write(void)
{
    uint8_t c[6];
    static const uint8_t raw[6] = { 0x0A, 0, 0, 0x00, 0x3C, 0x00 };
    static const uint8_t pre[6] = { 0x0A, 0, 0, 0x05, 0xEA, 0x80 };
    mp_cdb_write_raw(c, 60);
    T_MEM(c, raw, 6);
    mp_cdb_write_preamble(c, 1514);
    T_MEM(c, pre, 6);
}

static void test_cdb_misc(void)
{
    uint8_t c[6];
    static const uint8_t st[6] = { 0x09, 0, 0, 0, 18, 0 };
    static const uint8_t mc[6] = { 0x0D, 0, 0, 0, 6, 0 };
    static const uint8_t en[6] = { 0x0E, 0, 0, 0, 0, 0x80 };
    static const uint8_t dis[6] = { 0x0E, 0, 0, 0, 0, 0 };
    static const uint8_t wj[6] = { 0x1C, 0x05, 0, 0, 130, 0 };
    static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    mp_cdb_stats(c);
    T_MEM(c, st, 6);
    mp_cdb_mcast(c);
    T_MEM(c, mc, 6);
    mp_cdb_enable(c, 1);
    T_MEM(c, en, 6);
    mp_cdb_enable(c, 0);
    T_MEM(c, dis, 6);
    mp_cdb_wifi(c, MP_WIFI_JOIN, MP_WIFI_JOIN_LEN);
    T_MEM(c, wj, 6);
    mp_cdb_inquiry(c, 36);
    T_MEM(c, inq, 6);
}

static void test_inquiry_match(void)
{
    uint8_t r[36];
    memset(r, 0, sizeof(r));
    r[0] = 3;
    memcpy(r + 8, "Dayna   ", 8);
    memcpy(r + 16, "SCSI/Link       ", 16);
    memcpy(r + 32, "2.0f", 4);
    T_CHECK(mp_dev_inquiry_match(r, 36));
    T_CHECK(!mp_dev_inquiry_match(r, 31));
    memcpy(r + 16, "SCSI/Link2      ", 16);
    T_CHECK(!mp_dev_inquiry_match(r, 36));
    memcpy(r + 16, "SCSI/Link       ", 16);
    memcpy(r + 8, "AmigaNET", 8);      /* DRIVEINFO_AMIGAWIFI [fw BlueSCSI_config.h] */
    T_CHECK(!mp_dev_inquiry_match(r, 36));
    memcpy(r + 8, "QUANTUM ", 8);
    T_CHECK(!mp_dev_inquiry_match(r, 36));
}

/* The firmware's multi-packet stop rule [fw network.c case 0x08], fed packets
 * of len 'plen' (CRC included). Returns packets sent per READ. */
static unsigned fw_packets(uint32_t alloc, uint32_t plen, unsigned queued)
{
    unsigned n = 0;
    uint32_t total = 0;
    for (;;) {
        total += plen + 6;
        n++;
        if (n == queued)
            break;
        if (total + 1524 + 6 > alloc)
            break;
        if (total >= (1524 + 6) * 2)
            break;
    }
    return n;
}

static void test_read_alloc_safe(void)
{
    uint16_t k;
    T_EQ(mp_dev_read_alloc(1, 1), 1530);
    T_EQ(mp_dev_read_alloc(0, 1), 1530);
    T_EQ(mp_dev_read_alloc(4, 1), 1530 + 3 * 70);
    for (k = 1; k <= MP_RING_MAX; k++) {
        uint16_t a = mp_dev_read_alloc(k, 1);
        uint32_t plen;
        T_CHECK(a >= 1526);   /* old fw: no truncation of a 1520 packet */
        for (plen = 64; plen <= 1520; plen += 2)
            T_CHECK(fw_packets(a, plen, 19) <= k);
    }
}

static void test_read_alloc_greedy(void)
{
    T_EQ(mp_dev_read_alloc(1, 0), 1530);
    T_EQ(mp_dev_read_alloc(2, 0), 3060);
    T_EQ(fw_packets(mp_dev_read_alloc(2, 0), 1518, 19), 2);
    T_EQ(mp_dev_read_alloc(MP_RING_MAX, 0), (MP_RING_MAX * 1530u > 0xFFFFu) ? 0xFFFF : MP_RING_MAX * 1530u);
}

static void test_tx_hdr_pad(void)
{
    uint8_t h[4] = { 9, 9, 9, 9 };
    mp_dev_tx_hdr(h, 0x05EA);
    T_EQ(h[0], 0x05);
    T_EQ(h[1], 0xEA);
    T_EQ(h[2], 0);
    T_EQ(h[3], 0);
    T_EQ(mp_dev_tx_padlen(14), 60);
    T_EQ(mp_dev_tx_padlen(59), 60);
    T_EQ(mp_dev_tx_padlen(60), 60);
    T_EQ(mp_dev_tx_padlen(1514), 1514);
}

static void test_stats_parse(void)
{
    uint8_t r[18] = { 0x00, 0x80, 0x19, 1, 2, 3, 0, 0, 0, 7, 0, 0, 1, 0, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t mac[6];
    uint32_t c[3];
    T_EQ(mp_dev_parse_stats(r, 18, mac, c), MP_OK);
    T_EQ(mac[0], 0x00);
    T_EQ(mac[5], 3);
    T_EQ(c[0], 7);
    T_EQ(c[1], 256);
    T_CHECK(c[2] == 0xFFFFFFFFUL);
    T_EQ(mp_dev_parse_stats(r, 10, mac, c), MP_OK);
    T_EQ(c[0], 7);
    T_EQ(c[1], 0);
    T_EQ(mp_dev_parse_stats(r, 5, mac, NULL), MP_E_LEN);
}

static void test_wifi_codec(void)
{
    uint8_t r[2 + 2 * 74];
    const uint8_t *first;
    mp_wifi_entry e;
    memset(r, 0, sizeof(r));
    r[1] = 148;
    memset(r + 2, 'A', 64);   /* full 64-byte SSID, no NUL */
    r[2 + 64] = 0x11;
    r[2 + 70] = 0xC0;         /* rssi -64 */
    r[2 + 71] = 11;
    r[2 + 72] = MP_WIFI_FLAG_AUTH;
    T_EQ(mp_dev_wifi_list(r, sizeof(r), &first), 2);
    T_EQ(mp_dev_wifi_list(r, 2 + 74 + 10, &first), 1);   /* bounded by n */
    T_EQ(mp_dev_wifi_list(r, 1, &first), 0);
    mp_dev_wifi_entry(first, &e);
    T_EQ(strlen(e.ssid), 64);
    T_EQ(e.bssid[0], 0x11);
    T_EQ(e.rssi, -64);
    T_EQ(e.channel, 11);
    T_EQ(e.flags, 1);
}

static void test_wifi_join(void)
{
    uint8_t o[MP_WIFI_JOIN_LEN];
    char longs[100];
    memset(o, 0x55, sizeof(o));
    mp_dev_wifi_join(o, "MoofNet", "secret", 6);
    T_MEM(o, "MoofNet", 8);
    T_MEM(o + 64, "secret", 7);
    T_EQ(o[63], 0);
    T_EQ(o[128], 6);
    T_EQ(o[129], 0);
    memset(longs, 'x', sizeof(longs) - 1);
    longs[99] = 0;
    mp_dev_wifi_join(o, longs, NULL, 1);
    T_EQ(o[62], 'x');
    T_EQ(o[63], 0);   /* truncated + NUL */
    T_EQ(o[64], 0);
}

T_MAIN(test_cdb_read, test_cdb_write, test_cdb_misc, test_inquiry_match,
       test_read_alloc_safe, test_read_alloc_greedy, test_tx_hdr_pad, test_stats_parse,
       test_wifi_codec, test_wifi_join)

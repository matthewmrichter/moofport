/* test_rx.c: incremental READ-response parser, fed by hand. */
#include "t.h"
#include "mp_dev.h"

static uint8_t mem[MP_RING_BYTES(4)];
static mp_ring ring;
static mp_rx rx;

static void setup(uint16_t slots)
{
    mp_ring_init(&ring, mem, slots);
    mp_rx_init(&rx, &ring);
}

/* Append one packet (header + len bytes of pattern) to buf. */
static uint32_t put_pkt(uint8_t *buf, uint32_t at, uint16_t len, uint8_t flags, uint8_t seed)
{
    uint16_t i;
    buf[at] = (uint8_t)(len >> 8);
    buf[at + 1] = (uint8_t)len;
    buf[at + 2] = buf[at + 3] = buf[at + 4] = 0;
    buf[at + 5] = flags;
    for (i = 0; i < len; i++)
        buf[at + 6 + i] = (uint8_t)(seed + i);
    return at + 6u + len;
}

/* Feed 'n' bytes of buf in chunks of at most 'chunk' bytes; stops at
 * MP_RX_PKT/EMPTY. Returns bytes consumed. */
static uint32_t feed(const uint8_t *buf, uint32_t n, uint16_t chunk)
{
    uint32_t pos = 0;
    while (pos < n) {
        uint8_t *dst;
        uint16_t want, m, i;
        uint8_t s = mp_rx_want(&rx, &dst, &want);
        if (s != MP_RX_HDR && s != MP_RX_DATA && s != MP_RX_SKIP)
            break;
        m = want < chunk ? want : chunk;
        if (m > n - pos)
            m = (uint16_t)(n - pos);
        if (s == MP_RX_SKIP) {
            for (i = 0; i < m; i++)
                dst[i % MP_RX_SINK] = buf[pos + i];
        } else {
            memcpy(dst, buf + pos, m);
        }
        pos += m;
        mp_rx_got(&rx, m);
    }
    return pos;
}

static void test_empty(void)
{
    uint8_t z[6] = { 0 };
    setup(4);
    mp_rx_begin(&rx);
    T_EQ(feed(z, 6, 1), 6);
    T_EQ(mp_rx_want(&rx, NULL, NULL), MP_RX_EMPTY);
    T_EQ(mp_rx_end(&rx), 0);
    T_EQ(rx.st.empty, 1);
    T_EQ(rx.st.polls, 1);
    T_EQ(mp_ring_used(&ring), 0);
}

static void test_one_packet_bytewise(void)
{
    static uint8_t b[2000];
    uint32_t n = put_pkt(b, 0, 64, 0, 0x40);
    const uint8_t *f;
    uint16_t len;
    setup(4);
    mp_rx_begin(&rx);
    T_EQ(feed(b, n, 1), n);      /* one byte at a time: piecewise is fine */
    T_EQ(mp_rx_want(&rx, NULL, NULL), MP_RX_PKT);
    T_EQ(mp_rx_end(&rx), 0);
    f = mp_ring_peek(&ring, &len);
    T_CHECK(f != NULL);
    T_EQ(len, 60);               /* len counts 4 CRC bytes [fw] */
    T_EQ(f[0], 0x40);
    T_EQ(f[59], (uint8_t)(0x40 + 59));
    T_EQ(rx.st.frames, 1);
}

static void test_header_then_slot_pointer(void)
{
    /* The glue reads the header polled, then the payload straight into the slot. */
    uint8_t hdr[6] = { 0x05, 0xEE, 0, 0, 0, 0 };   /* 1518 = 1514 + CRC */
    uint8_t *dst;
    uint16_t n;
    setup(4);
    mp_rx_begin(&rx);
    T_EQ(mp_rx_want(&rx, &dst, &n), MP_RX_HDR);
    T_EQ(n, 6);
    memcpy(dst, hdr, 6);
    T_EQ(mp_rx_got(&rx, 6), MP_RX_DATA);
    T_EQ(mp_rx_want(&rx, &dst, &n), MP_RX_DATA);
    T_EQ(n, 1518);
    T_CHECK(dst == mp_ring_claim(&ring));   /* zero copy: the slot itself */
    memset(dst, 0xA5, 8);                   /* polled lead */
    T_EQ(mp_rx_got(&rx, 8), MP_RX_DATA);
    mp_rx_want(&rx, &dst, &n);
    T_EQ(n, 1510);
    memset(dst, 0x5A, n);                   /* blind remainder */
    T_EQ(mp_rx_got(&rx, n), MP_RX_PKT);
    T_EQ(mp_ring_used(&ring), 1);
    {
        uint16_t len;
        const uint8_t *f = mp_ring_peek(&ring, &len);
        T_EQ(len, 1514);
        T_EQ(f[7], 0xA5);
        T_EQ(f[8], 0x5A);
    }
}

static void test_multi_and_more(void)
{
    static uint8_t b[4000];
    uint32_t n = 0;
    n = put_pkt(b, n, 100, 0x10, 1);
    n = put_pkt(b, n, 200, 0x10, 2);
    setup(4);
    mp_rx_begin(&rx);
    feed(b, n, 64);
    T_EQ(mp_rx_want(&rx, NULL, NULL), MP_RX_PKT);
    mp_rx_next(&rx);   /* phase still DATA IN */
    feed(b + 106, n - 106, 64);
    T_EQ(mp_rx_want(&rx, NULL, NULL), MP_RX_PKT);
    /* last header said 0x10 but the phase went to STATUS: poll again now */
    T_EQ(mp_rx_end(&rx), 1);
    T_EQ(rx.st.more_ended, 1);
    T_EQ(rx.st.multi, 1);
    T_EQ(rx.saw_multi, 1);
    T_EQ(mp_ring_used(&ring), 2);
}

static void test_bad_and_oversize_skipped(void)
{
    static uint8_t b[4000];
    uint32_t n = 0;
    uint8_t *dst;
    uint16_t w;
    n = put_pkt(b, n, 10, 0x10, 0);      /* below 14 + 4 */
    setup(4);
    mp_rx_begin(&rx);
    feed(b, 6, 6);
    T_EQ(mp_rx_want(&rx, &dst, &w), MP_RX_SKIP);
    T_CHECK(dst == rx.sink);
    T_EQ(w, 10);
    feed(b + 6, n - 6, 3);
    T_EQ(mp_rx_want(&rx, NULL, NULL), MP_RX_PKT);
    T_EQ(rx.st.bad, 1);
    mp_rx_next(&rx);
    n = put_pkt(b, 0, 1600, 0, 0);       /* above 1520 */
    feed(b, n, 512);
    T_EQ(mp_rx_want(&rx, NULL, NULL), MP_RX_PKT);
    T_EQ(rx.st.oversize, 1);
    mp_rx_end(&rx);
    T_EQ(mp_ring_used(&ring), 0);
    T_EQ(rx.st.frames, 0);
}

static void test_overflow_skips(void)
{
    static uint8_t b[4000];
    uint32_t n = 0, i;
    setup(1);
    mp_rx_begin(&rx);
    for (i = 0; i < 3; i++) {
        n = put_pkt(b, 0, 70, i < 2 ? 0x10 : 0, (uint8_t)i);
        feed(b, n, 1000);
        T_EQ(mp_rx_want(&rx, NULL, NULL), MP_RX_PKT);
        if (i < 2)
            mp_rx_next(&rx);
    }
    T_EQ(mp_rx_end(&rx), 0);
    T_EQ(rx.st.frames, 1);
    T_EQ(rx.st.overflow, 2);
    T_EQ(mp_ring_used(&ring), 1);
}

static void test_abort_commits_nothing(void)
{
    static uint8_t b[2000];
    uint32_t n = put_pkt(b, 0, 500, 0, 0);
    setup(2);
    mp_rx_begin(&rx);
    feed(b, n - 100, 64);
    T_EQ(mp_rx_want(&rx, NULL, NULL), MP_RX_DATA);
    mp_rx_abort(&rx);
    T_EQ(mp_rx_want(&rx, NULL, NULL), MP_RX_IDLE);
    T_EQ(mp_ring_used(&ring), 0);
    T_EQ(rx.st.aborted, 1);
    /* next transaction reuses the slot */
    mp_rx_begin(&rx);
    feed(b, n, 1000);
    mp_rx_end(&rx);
    T_EQ(mp_ring_used(&ring), 1);
}

static void test_got_clamps_and_idle(void)
{
    uint8_t hdr[6] = { 0, 64, 0, 0, 0, 0 };
    setup(2);
    T_EQ(mp_rx_got(&rx, 5), MP_RX_IDLE);   /* no transaction: ignored */
    mp_rx_begin(&rx);
    memcpy(rx.hdr, hdr, 6);
    T_EQ(mp_rx_got(&rx, 600), MP_RX_DATA); /* clamped to the 6 header bytes */
    T_EQ(rx.need, 64);
}

T_MAIN(test_empty, test_one_packet_bytewise, test_header_then_slot_pointer,
       test_multi_and_more, test_bad_and_oversize_skipped, test_overflow_skips,
       test_abort_commits_nothing, test_got_clamps_and_idle)

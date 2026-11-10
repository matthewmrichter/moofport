/* test_tx.c: TX deferral queue and the WRITE byte layout. */
#include "t.h"
#include "mp_dev.h"
#include "fakedev.h"

static uint8_t qmem[MP_RING_BYTES(2)];
static mp_ring q;
static mp_tx tx;
static fakedev dev;
static mp_bus bus;
static const uint8_t mac[6] = { 0x00, 0x80, 0x19, 0xAA, 0xBB, 0xCC };

static void setup(int kind, int batch)
{
    mp_ring_init(&q, qmem, 2);
    mp_tx_init(&tx, &q, mac, batch);
    fd_init(&dev, kind);
    fd_bus(&dev, &bus);
}

static void test_queue_pads_and_sets_mac(void)
{
    uint8_t a[14], b[28];
    mp_seg s[2];
    const uint8_t *f;
    uint16_t len, i;
    setup(FD_FW_2608, 0);
    memset(a, 0xFF, sizeof(a));
    memset(b, 0x11, sizeof(b));
    s[0].p = a; s[0].len = 14;
    s[1].p = b; s[1].len = 28;
    T_EQ(mp_tx_queue(&tx, s, 2), MP_OK);
    f = mp_ring_peek(&q, &len);
    T_EQ(len, 60);
    T_MEM(f + 6, mac, 6);
    T_EQ(f[0], 0xFF);
    T_EQ(f[14], 0x11);
    T_EQ(f[41], 0x11);
    for (i = 42; i < 60; i++)
        T_EQ(f[i], 0);
    T_EQ(a[6], 0xFF);   /* queue copies: caller's bytes untouched */
    T_EQ(tx.st.queued, 1);
}

static void test_queue_len_and_full(void)
{
    static uint8_t big[1515];
    mp_seg s;
    setup(FD_FW_2608, 0);
    s.p = big;
    s.len = 13;
    T_EQ(mp_tx_queue(&tx, &s, 1), MP_E_LEN);
    s.len = 1515;
    T_EQ(mp_tx_queue(&tx, &s, 1), MP_E_LEN);
    T_EQ(tx.st.badlen, 2);
    s.len = 1514;
    T_EQ(mp_tx_queue(&tx, &s, 1), MP_OK);
    T_EQ(mp_tx_queue(&tx, &s, 1), MP_OK);
    T_EQ(mp_tx_queue(&tx, &s, 1), MP_E_FULL);
    T_EQ(tx.st.full, 1);
}

/* Bytes on the bus for the universal single-frame write. */
static uint8_t wire[2000];
static uint32_t wire_n;
static uint8_t wcdb[6];
static int r_begin(void *c, const uint8_t *cdb) { (void)c; memcpy(wcdb, cdb, 6); wire_n = 0; return 0; }
static int r_xin(void *c, uint8_t *d, uint16_t n, uint8_t h) { (void)c; (void)d; (void)n; (void)h; return MP_E_BUS; }
static int r_xout(void *c, const uint8_t *s, uint16_t n, uint8_t h)
{
    (void)c; (void)h;
    memcpy(wire + wire_n, s, n);
    wire_n += n;
    return 0;
}
static int r_in_data(void *c) { (void)c; return 0; }
static int r_end(void *c) { (void)c; return 0; }

static void test_universal_layout(void)
{
    mp_bus rb = { NULL, r_begin, r_xin, r_xout, r_in_data, r_end };
    uint8_t f[42];
    mp_seg s;
    uint32_t i;
    setup(FD_FW_2608, 0);
    memset(f, 0x77, sizeof(f));
    s.p = f;
    s.len = sizeof(f);
    T_EQ(mp_tx_send(&tx, &rb, &s, 1), MP_OK);
    /* CDB: WRITE(6), len = padded 60, CDB[5] = 0x80 */
    T_EQ(wcdb[0], 0x0A);
    T_EQ(mp_rd16(wcdb + 3), 60);
    T_EQ(wcdb[5], 0x80);
    /* [00 3C 00 00][frame 42][18 zero pad][00 00 00 00] */
    T_EQ(wire_n, 4 + 60 + 4);
    T_EQ(mp_rd16(wire), 60);
    T_EQ(wire[2], 0);
    T_EQ(wire[3], 0);
    T_MEM(wire + 4 + 6, mac, 6);   /* source MAC written into the frame at +6 */
    T_EQ(wire[4 + 41], 0x77);
    for (i = 4 + 42; i < wire_n; i++)
        T_EQ(wire[i], 0);
}

static void test_send_needs_header_piece(void)
{
    uint8_t a[8], b[60];
    mp_seg s[2];
    setup(FD_FW_2608, 0);
    s[0].p = a; s[0].len = 8;   /* first piece too short to hold the source MAC */
    s[1].p = b; s[1].len = 60;
    T_EQ(mp_tx_send(&tx, &bus, s, 2), MP_E_LEN);
    T_EQ(mp_tx_queue(&tx, s, 2), MP_OK);   /* the copy path handles it */
}

static void test_flush_empty(void)
{
    setup(FD_FW_2608, 1);
    T_EQ(mp_tx_flush(&tx, &bus), 0);
    T_EQ(dev.cmds, 0);
}

T_MAIN(test_queue_pads_and_sets_mac, test_queue_len_and_full, test_universal_layout,
       test_send_needs_header_piece, test_flush_empty)

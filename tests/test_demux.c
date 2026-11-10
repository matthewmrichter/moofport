/* test_demux.c: EAttachPH / EDetachPH semantics and EtherType lookup. */
#include "t.h"
#include "mp_demux.h"

static int h_ip, h_arp, h_8023;

static void frame_type(uint8_t *f, uint16_t t)
{
    memset(f, 0, 60);
    f[12] = (uint8_t)(t >> 8);
    f[13] = (uint8_t)t;
}

static void test_attach_dup(void)
{
    mp_demux d;
    mp_demux_init(&d);
    T_EQ(mp_demux_attach(&d, 0x0800, &h_ip), 0);
    T_EQ(mp_demux_attach(&d, 0x0800, &h_arp), MP_LAP_PROT_ERR);   /* -94 */
    T_EQ(MP_LAP_PROT_ERR, -94);
    T_CHECK(mp_demux_find(&d, 0x0800)->handler == &h_ip);
}

static void test_detach(void)
{
    mp_demux d;
    mp_demux_init(&d);
    T_EQ(mp_demux_detach(&d, 0x0806), MP_LAP_PROT_ERR);
    T_EQ(mp_demux_attach(&d, 0x0806, &h_arp), 0);
    T_EQ(mp_demux_detach(&d, 0x0806), 0);
    T_CHECK(mp_demux_find(&d, 0x0806) == NULL);
    T_EQ(mp_demux_attach(&d, 0x0806, &h_ip), 0);   /* re-attach after detach */
}

static void test_full(void)
{
    mp_demux d;
    uint16_t i;
    mp_demux_init(&d);
    for (i = 0; i < MP_DEMUX_MAX; i++)
        T_EQ(mp_demux_attach(&d, (uint16_t)(0x9000 + i), NULL), 0);
    T_EQ(mp_demux_attach(&d, 0x0800, &h_ip), MP_LAP_PROT_ERR);
    T_EQ(mp_demux_detach(&d, 0x9003), 0);
    T_EQ(mp_demux_attach(&d, 0x0800, &h_ip), 0);
}

static void test_lookup_and_8023(void)
{
    mp_demux d;
    uint8_t f[60];
    mp_ph *e;
    mp_demux_init(&d);
    mp_demux_attach(&d, 0x0800, &h_ip);
    mp_demux_attach(&d, MP_ETYPE_8023, &h_8023);
    frame_type(f, 0x0800);
    e = mp_demux_lookup(&d, f, 60);
    T_CHECK(e && e->handler == &h_ip);
    frame_type(f, 0x0806);
    T_CHECK(mp_demux_lookup(&d, f, 60) == NULL);
    T_EQ(d.unattached, 1);
    frame_type(f, 46);        /* 802.3 length field */
    e = mp_demux_lookup(&d, f, 60);
    T_CHECK(e && e->handler == &h_8023);
    frame_type(f, 1500);
    T_CHECK(mp_demux_lookup(&d, f, 60) == e);
    frame_type(f, 1501);
    T_CHECK(mp_demux_lookup(&d, f, 60) == NULL);
    T_EQ(e->frames, 2);
    T_EQ(mp_demux_find(&d, 0x0800)->frames, 1);
    T_CHECK(mp_demux_lookup(&d, f, 13) == NULL);
    T_EQ(d.runts, 1);
    T_EQ(d.unattached, 2);
}

static void test_null_handler_allowed(void)
{
    mp_demux d;
    uint8_t f[60];
    mp_ph *e;
    mp_demux_init(&d);
    T_EQ(mp_demux_attach(&d, 0x809B, NULL), 0);   /* ERead path */
    frame_type(f, 0x809B);
    e = mp_demux_lookup(&d, f, 60);
    T_CHECK(e != NULL && e->handler == NULL);
}

T_MAIN(test_attach_dup, test_detach, test_full, test_lookup_and_8023, test_null_handler_allowed)

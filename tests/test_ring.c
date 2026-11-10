/* test_ring.c: fixed-slot SPSC ring. */
#include "t.h"
#include "mp_ring.h"

static uint8_t mem[MP_RING_BYTES(4)];

static void test_init_args(void)
{
    mp_ring r;
    T_EQ(mp_ring_init(&r, mem, 0), MP_E_ARG);
    T_EQ(mp_ring_init(&r, NULL, 2), MP_E_ARG);
    T_EQ(mp_ring_init(&r, mem, MP_RING_MAX + 1), MP_E_ARG);
    T_EQ(mp_ring_init(&r, mem, 4), MP_OK);
    T_EQ(mp_ring_used(&r), 0);
    T_EQ(mp_ring_free(&r), 4);
    T_CHECK(mp_ring_peek(&r, NULL) == NULL);
}

static void test_fill_drain(void)
{
    mp_ring r;
    uint8_t *s;
    const uint8_t *p;
    uint16_t len, i;
    mp_ring_init(&r, mem, 4);
    for (i = 0; i < 4; i++) {
        s = mp_ring_claim(&r);
        T_CHECK(s != NULL);
        T_CHECK(((s - mem) % MP_SLOT_SIZE) == MP_SLOT_HDR);
        s[0] = (uint8_t)i;
        mp_ring_commit(&r, (uint16_t)(100 + i));
    }
    T_CHECK(mp_ring_claim(&r) == NULL);   /* full, no slot wasted */
    T_EQ(mp_ring_used(&r), 4);
    T_EQ(mp_ring_free(&r), 0);
    p = mp_ring_peek_at(&r, 3, &len);
    T_CHECK(p && p[0] == 3 && len == 103);
    T_CHECK(mp_ring_peek_at(&r, 4, &len) == NULL);
    for (i = 0; i < 4; i++) {
        p = mp_ring_peek(&r, &len);
        T_CHECK(p != NULL);
        T_EQ(p[0], i);
        T_EQ(len, 100 + i);
        mp_ring_pop(&r);
    }
    T_EQ(mp_ring_used(&r), 0);
}

static void test_claim_twice_same_slot(void)
{
    mp_ring r;
    mp_ring_init(&r, mem, 2);
    T_CHECK(mp_ring_claim(&r) == mp_ring_claim(&r));
    T_EQ(mp_ring_used(&r), 0);   /* claim alone publishes nothing */
}

static void test_wrap_many(void)
{
    mp_ring r;
    uint32_t i, wseq = 0, rseq = 0;
    mp_ring_init(&r, mem, 3);
    for (i = 0; i < 1000; i++) {
        /* producer adds 0..2, consumer removes 0..2, interleaved */
        uint32_t k, add = i % 3, del = (i * 7) % 3;
        for (k = 0; k < add; k++) {
            uint8_t *s = mp_ring_claim(&r);
            if (!s)
                break;
            mp_wr16(s, (uint16_t)wseq);
            mp_ring_commit(&r, (uint16_t)(wseq % MP_SLOT_DATA));
            wseq++;
        }
        for (k = 0; k < del; k++) {
            uint16_t len;
            const uint8_t *p = mp_ring_peek(&r, &len);
            if (!p)
                break;
            T_EQ(mp_rd16(p), (uint16_t)rseq);
            T_EQ(len, rseq % MP_SLOT_DATA);
            mp_ring_pop(&r);
            rseq++;
        }
        T_EQ(mp_ring_used(&r), wseq - rseq);
        T_CHECK(mp_ring_used(&r) <= 3);
    }
}

static void test_pop_n_and_len_clamp(void)
{
    mp_ring r;
    uint16_t len;
    mp_ring_init(&r, mem, 4);
    mp_ring_claim(&r);
    mp_ring_commit(&r, 60000);   /* clamped to the slot */
    mp_ring_peek(&r, &len);
    T_EQ(len, MP_SLOT_DATA);
    mp_ring_claim(&r);
    mp_ring_commit(&r, 1);
    mp_ring_pop_n(&r, 9);        /* clamped to used */
    T_EQ(mp_ring_used(&r), 0);
    mp_ring_claim(&r);
    mp_ring_commit(&r, 1);
    mp_ring_reset(&r);
    T_EQ(mp_ring_used(&r), 0);
}

static void test_slot_holds_max_packet(void)
{
    /* the largest device packet (1520 incl. CRC) fits a slot [fw network.h] */
    T_EQ(MP_SLOT_DATA, 1520);
    T_EQ(MP_SLOT_SIZE, 1524);
}

T_MAIN(test_init_args, test_fill_drain, test_claim_twice_same_slot, test_wrap_many,
       test_pop_n_and_len_clamp, test_slot_holds_max_packet)

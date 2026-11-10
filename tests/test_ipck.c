/* test_ipck.c: mp_ip_check against frames whose checksums were computed in Python
 * (tests/ipvec.h, tools/gen-ipvec.py). */
#include <string.h>
#include "t.h"
#include "mp_ipck.h"
#include "ipvec.h"

static uint8_t g_f[1600];

static void test_valid_frames(void)
{
    T_EQ(mp_ip_check(k_tcp, sizeof k_tcp), MP_IPCK_FULL);
    T_EQ(mp_ip_check(k_tcp_small, sizeof k_tcp_small), MP_IPCK_FULL);
    T_EQ(mp_ip_check(k_udp_odd, sizeof k_udp_odd), MP_IPCK_FULL);   /* odd payload length */
}

static void test_padding_and_crc_after_datagram(void)
{
    /* the device's length counts 4 CRC bytes, and short frames are padded to 60 */
    memset(g_f, 0xEE, sizeof g_f);
    memcpy(g_f, k_tcp_small, sizeof k_tcp_small);
    T_EQ(mp_ip_check(g_f, (uint16_t)(sizeof k_tcp_small + 4)), MP_IPCK_FULL);
    T_EQ(mp_ip_check(g_f, 64), MP_IPCK_FULL);
    memcpy(g_f, k_tcp, sizeof k_tcp);
    T_EQ(mp_ip_check(g_f, (uint16_t)(sizeof k_tcp + 4)), MP_IPCK_FULL);
}

static void test_any_flipped_byte_is_bad(void)
{
    uint16_t i;
    int bad = 0;
    /* every byte of the IP header and the TCP segment, one at a time */
    for (i = 14; i < sizeof k_tcp; i++) {
        memcpy(g_f, k_tcp, sizeof k_tcp);
        g_f[i] ^= 0x5A;
        if (mp_ip_check(g_f, sizeof k_tcp) == MP_IPCK_BAD)
            bad++;
        else                           /* only the version nibble: no longer IPv4 at all */
            T_EQ(mp_ip_check(g_f, sizeof k_tcp), i == 14 ? MP_IPCK_NONE : MP_IPCK_BAD);
    }
    T_EQ(bad, (int)sizeof k_tcp - 15);
}

static void test_truncated_is_bad(void)
{
    T_EQ(mp_ip_check(k_tcp, (uint16_t)(sizeof k_tcp - 1)), MP_IPCK_BAD);
    T_EQ(mp_ip_check(k_udp_odd, (uint16_t)(sizeof k_udp_odd - 10)), MP_IPCK_BAD);
}

static void test_not_ip_and_unchecked(void)
{
    memcpy(g_f, k_udp_odd, sizeof k_udp_odd);
    g_f[12] = 0x08; g_f[13] = 0x06;                       /* ARP */
    T_EQ(mp_ip_check(g_f, sizeof k_udp_odd), MP_IPCK_NONE);
    T_EQ(mp_ip_check(k_tcp, 20), MP_IPCK_NONE);           /* too short to be IPv4 */
    memcpy(g_f, k_udp_odd, sizeof k_udp_odd);
    g_f[14 + 20 + 6] = 0; g_f[14 + 20 + 7] = 0;           /* UDP checksum 0 = none */
    T_EQ(mp_ip_check(g_f, sizeof k_udp_odd), MP_IPCK_HDR);
}

T_MAIN(test_valid_frames, test_padding_and_crc_after_datagram, test_any_flipped_byte_is_bad,
       test_truncated_is_bad, test_not_ip_and_unchecked)

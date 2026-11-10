/* mp_ipck.c: IPv4 / TCP / UDP checksum check (see mp_ipck.h). */
#include "mp_ipck.h"

static uint32_t sum16(const uint8_t *p, uint16_t n, uint32_t s)
{
    for (; n > 1; n = (uint16_t)(n - 2), p += 2)
        s += mp_rd16(p);
    if (n)
        s += (uint32_t)p[0] << 8;
    return s;
}

static uint16_t fold(uint32_t s)
{
    while (s >> 16)
        s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)s;
}

int mp_ip_check(const uint8_t *f, uint16_t len)
{
    const uint8_t *ip = f + 14, *seg;
    uint16_t ihl, tot, sl;
    uint8_t proto;
    if (len < 14 + 20 || mp_rd16(f + 12) != 0x0800 || (ip[0] >> 4) != 4)
        return MP_IPCK_NONE;
    ihl = (uint16_t)((ip[0] & 15u) * 4u);
    tot = mp_rd16(ip + 2);
    /* the frame must hold the whole datagram; padding and CRC after it are fine */
    if (ihl < 20 || tot < ihl || (uint32_t)14 + tot > len || fold(sum16(ip, ihl, 0)) != 0xFFFF)
        return MP_IPCK_BAD;
    proto = ip[9];
    seg = ip + ihl;
    sl = (uint16_t)(tot - ihl);
    if ((mp_rd16(ip + 6) & 0x3FFFu) || (proto != 6 && proto != 17))
        return MP_IPCK_HDR;
    if (proto == 17 && (sl < 8 || mp_rd16(seg + 6) == 0))
        return sl < 8 ? MP_IPCK_BAD : MP_IPCK_HDR;
    if (proto == 6 && sl < 20)
        return MP_IPCK_BAD;
    /* pseudo-header: source, destination, protocol, TCP/UDP length */
    if (fold(sum16(seg, sl, sum16(ip + 12, 8, (uint32_t)proto + sl))) != 0xFFFF)
        return MP_IPCK_BAD;
    return MP_IPCK_FULL;
}

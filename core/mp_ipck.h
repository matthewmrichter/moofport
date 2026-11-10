/* mp_ipck.h: IPv4 / TCP / UDP checksum check of a received frame (Phase 3).
 *
 * Used only in the driver's verify mode, the hardware gate for blind SCSI reads (PLAN 2.2): with
 * blind payload reads on, ordinary TCP traffic then shows whether any byte arrived wrong. Not
 * on the normal receive path. One's-complement sums over 16-bit big-endian words, RFC 1071.
 */
#ifndef MP_IPCK_H
#define MP_IPCK_H

#include "mp_util.h"

#define MP_IPCK_NONE 0   /* not an IPv4 frame (ARP, AppleTalk, ...): nothing checked */
#define MP_IPCK_HDR  1   /* IPv4 header right; payload not checkable (fragment, not TCP/UDP,
                          * UDP without a checksum) */
#define MP_IPCK_FULL 2   /* IPv4 header and TCP or UDP checksum right */
#define MP_IPCK_BAD  3   /* a checksum is wrong, or the lengths do not fit the frame */

/* f = Ethernet frame, len = bytes available (may include padding and the 4 CRC bytes). */
int mp_ip_check(const uint8_t *f, uint16_t len);

#endif

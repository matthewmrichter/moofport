/* mp_ring.h: fixed-slot frame ring (RX slots and the TX deferral queue).
 *
 * One ring = 'count' slots of MP_SLOT_SIZE bytes in caller memory. Slot layout:
 *   +0  uint16 big-endian frame length (written by the producer at commit)
 *   +2  2 bytes reserved (0)
 *   +4  frame bytes, up to MP_SLOT_DATA (1520 = the firmware's
 *       NETWORK_PACKET_MAX_SIZE, so a device packet including its 4 CRC
 *       bytes always fits; network.h)
 *
 * Concurrency: single producer, single consumer. One side may run at interrupt
 * time and the other at task time (for example the poll task fills RX slots,
 * a task drains them; EWrite queues TX slots, the poll task flushes them).
 * 'head' is written only by the producer, 'tail' only by the consumer, both are
 * volatile 16-bit values (a single 68000 word store), and the slot bytes are
 * ordered before the publishing index store by MP_BARRIER. More than one
 * producer (EWrite from task and from a nested handler) must be serialised by
 * the caller (mask interrupts around claim+commit).
 *
 * Indices run over 0..2*count-1 so a full ring is told apart from an empty one
 * without wasting a slot and without a division on the hot path.
 */
#ifndef MP_RING_H
#define MP_RING_H

#include "mp_util.h"

#define MP_SLOT_SIZE   1524u
#define MP_SLOT_HDR    4u
#define MP_SLOT_DATA   (MP_SLOT_SIZE - MP_SLOT_HDR)   /* 1520 */
#define MP_RING_MAX    32u   /* upper bound on slots per ring */

typedef struct mp_ring {
    uint8_t *mem;               /* count * MP_SLOT_SIZE bytes, caller-owned */
    uint16_t count;
    volatile uint16_t head;     /* producer: next slot to fill (0..2*count-1) */
    volatile uint16_t tail;     /* consumer: next slot to read (0..2*count-1) */
} mp_ring;

/* mem must hold count * MP_SLOT_SIZE bytes, count 1..MP_RING_MAX, 2-byte aligned. */
int mp_ring_init(mp_ring *r, uint8_t *mem, uint16_t count);

/* Discard everything. Only when neither side is active. */
void mp_ring_reset(mp_ring *r);

uint16_t mp_ring_used(const mp_ring *r);
uint16_t mp_ring_free(const mp_ring *r);

/* Producer: data area of the next free slot (MP_SLOT_DATA bytes) or NULL if
 * full. The slot is not visible to the consumer until mp_ring_commit. Claiming
 * twice without a commit returns the same slot. */
uint8_t *mp_ring_claim(mp_ring *r);
/* Producer: publish the claimed slot with 'len' frame bytes (<= MP_SLOT_DATA). */
void mp_ring_commit(mp_ring *r, uint16_t len);

/* Consumer: the i-th oldest committed frame (0 = oldest) or NULL; *len set. */
const uint8_t *mp_ring_peek_at(const mp_ring *r, uint16_t i, uint16_t *len);
#define mp_ring_peek(r, len) mp_ring_peek_at((r), 0, (len))
/* Consumer: release the n oldest frames (n <= used). */
void mp_ring_pop_n(mp_ring *r, uint16_t n);
#define mp_ring_pop(r) mp_ring_pop_n((r), 1)

/* Bytes of caller memory for a ring of n slots. */
#define MP_RING_BYTES(n) ((uint32_t)(n) * MP_SLOT_SIZE)

#endif

/* mp_demux.h: EtherType -> attached protocol handler table (EAttachPH / EDetachPH).
 *
 * Semantics (Inside Macintosh: Networking, EAttachPH / EDetachPH; PLAN 1.4):
 * - EAttachPH with a type already attached returns LAPProtErr (-94). A full
 *   table also returns -94 (the error IM documents for "can't attach").
 * - Protocol type 0 receives every 802.3 frame, i.e. any frame whose
 *   type/length field is <= 1500 (0x05DC).
 * - The handler pointer is opaque here (the glue stores the 68k routine
 *   address). NULL is allowed: the glue then routes the type to ERead.
 * - EDetachPH of a type that is not attached returns -94.
 * Concurrency: attach/detach run at task time, lookup at interrupt time. An
 * entry is filled before 'used' is set and 'used' is cleared before the entry
 * is reused, so lookup never sees a half-written entry.
 */
#ifndef MP_DEMUX_H
#define MP_DEMUX_H

#include "mp_util.h"

#define MP_LAP_PROT_ERR  (-94)     /* LAPProtErr */
#define MP_DEMUX_MAX     8u
#define MP_ETYPE_8023    0u        /* attach type for 802.3 (length) frames */
#define MP_ETYPE_MAXLEN  0x05DCu   /* 1500: type field values up to this are lengths */

typedef struct mp_ph {
    void *handler;
    uint32_t frames;               /* frames looked up to this entry */
    uint16_t type;
    volatile uint8_t used;
    uint8_t pad;
} mp_ph;

typedef struct mp_demux {
    mp_ph e[MP_DEMUX_MAX];
    uint32_t unattached;           /* frames with no attached type */
    uint32_t runts;                /* frames shorter than 14 bytes */
} mp_demux;

void mp_demux_init(mp_demux *d);
int mp_demux_attach(mp_demux *d, uint16_t type, void *handler);
int mp_demux_detach(mp_demux *d, uint16_t type);
/* Attached entry for a type (no counting), or NULL. */
mp_ph *mp_demux_find(mp_demux *d, uint16_t type);
/* The attach type a frame maps to: its EtherType, or 0 if that field is a length. */
uint16_t mp_demux_type(const uint8_t *frame);
/* Entry for a received frame, counting it, or NULL (counted as unattached/runt). */
mp_ph *mp_demux_lookup(mp_demux *d, const uint8_t *frame, uint16_t len);

#endif

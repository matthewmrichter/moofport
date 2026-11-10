/* mp_util.h: byte access, wrap-safe time, small helpers for the MoofPort core.
 *
 * Portable C99. Must also build with Retro68 m68k-apple-macos-gcc -m68000:
 * no GNU extensions outside the guarded barrier, no 64-bit math, no struct
 * overlays on packet bytes (the 68000 faults on odd-address word access).
 */
#ifndef MP_UTIL_H
#define MP_UTIL_H

#include <stdint.h>
#include <stddef.h>

/* Big-endian field access on byte pointers. Always widen before shifting. */
static inline uint16_t mp_rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t mp_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void mp_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void mp_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Wrap-safe uint32 ms clock: true if a is strictly before b. Valid while the
 * two values are less than 2^31 ms apart. Never compare clock values with < or >. */
static inline int mp_time_before(uint32_t a, uint32_t b)
{
    return ((uint32_t)(a - b) & 0x80000000UL) != 0;
}

/* Elapsed ms from 'then' to 'now' (wrap-safe; 0 if 'now' is before 'then'). */
static inline uint32_t mp_time_since(uint32_t now, uint32_t then)
{
    return mp_time_before(now, then) ? 0 : (uint32_t)(now - then);
}

/* Compiler barrier: orders plain memory accesses around the volatile ring
 * indices (a slot's bytes must be written before the index that publishes it).
 * The 68000 is in-order, so a compiler barrier is enough there. */
#if defined(__GNUC__)
#define MP_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define MP_BARRIER() ((void)0)
#endif

/* Core result codes (negative). Mac error codes appear only where the Mac
 * API defines them (mp_demux: LAPProtErr). */
#define MP_OK          0
#define MP_E_BUSY     (-1)   /* bus busy / SCSI Manager in use: retry later */
#define MP_E_BUS      (-2)   /* transfer failed (phase mismatch, timeout) */
#define MP_E_STATUS   (-3)   /* target ended with a non-GOOD status */
#define MP_E_FULL     (-4)   /* no free slot */
#define MP_E_LEN      (-5)   /* frame length out of range */
#define MP_E_ARG      (-6)   /* bad argument */

#endif

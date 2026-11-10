/* mp_budget.h: resident memory budget (PLAN 2.4).
 *
 * The cap is DaynaPORT's resident footprint, 16,554 bytes of fixed system heap
 * measured on the real Classic and in Snow (docs/dev/BENCH.md, hw1c), plus a 1,024-byte
 * allowance accepted by the project (PLAN section 7, decision 4: performance first, up to about
 * 1 KB over DaynaPORT). The cap still guards against creep.
 * The default build is 17,520 bytes:
 *   code <= 9400 (the 1.0b1 System-file DRVR is 9,388), RX ring 3 x 1524, TX deferral
 *   2 x 1524, state <= 500 (1.0b1: 448). Snow measured 17,304 fixed bytes for the opened Phase 3 driver
 *   (hw1c method; Phase 2 was 16,388).
 * Slot counts come from prefs, so the build shrinks if the cap is smaller.
 * test_budget fails the host build if the default configuration (or the
 * configuration set through CMake) exceeds MP_MEM_CAP.
 */
#ifndef MP_BUDGET_H
#define MP_BUDGET_H

#include "mp_util.h"

#define MP_BUDGET_DAYNA        16554u   /* DaynaPORT 1.2.5, hw1c */
#define MP_BUDGET_MARGIN       1024u    /* decision 4 */
#define MP_BUDGET_CAP_DEFAULT  (MP_BUDGET_DAYNA + MP_BUDGET_MARGIN)
#define MP_BUDGET_CODE_DEFAULT 9400u
#define MP_BUDGET_STATE_DEFAULT 500u
#define MP_BUDGET_RX_DEFAULT   3u
#define MP_BUDGET_TX_DEFAULT   2u

typedef struct mp_budget_cfg {
    uint16_t rx_slots;
    uint16_t tx_slots;
    uint16_t code_bytes;     /* driver code allowance (C + asm, flat build) */
    uint16_t state_bytes;    /* state, TIBs, stats, log ring */
} mp_budget_cfg;

void mp_budget_defaults(mp_budget_cfg *c);
/* code + (rx + tx) * 1524 + state. */
uint32_t mp_budget_total(const mp_budget_cfg *c);
/* Host-side size of the core state structs one driver instance holds (two
 * rings, RX parser, TX state, demux, poll). On a 64-bit host this is an upper
 * bound for the 68k (pointers are 8 bytes here, 4 there). */
uint32_t mp_budget_core_state(void);
/* 1 if the total fits under cap. */
int mp_budget_fits(const mp_budget_cfg *c, uint32_t cap);

#endif

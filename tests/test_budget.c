/* test_budget.c: resident memory budget (PLAN 2.4). Fails the build if the
 * configured slot counts exceed the cap. Cap and counts come from CMake:
 * -DMP_MEM_CAP=..., -DMP_RX_SLOTS=..., -DMP_TX_SLOTS=... */
#include "t.h"
#include "mp_budget.h"
#include "mp_ring.h"

#ifndef MP_MEM_CAP
#define MP_MEM_CAP MP_BUDGET_CAP_DEFAULT
#endif
#ifndef MP_RX_SLOTS
#define MP_RX_SLOTS MP_BUDGET_RX_DEFAULT
#endif
#ifndef MP_TX_SLOTS
#define MP_TX_SLOTS MP_BUDGET_TX_DEFAULT
#endif

static void test_plan_table(void)
{
    mp_budget_cfg c;
    mp_budget_defaults(&c);
    /* 9400 + 3 x 1524 + 2 x 1524 + 500 = 17520 (PLAN 2.4) */
    T_EQ(mp_budget_total(&c), 17520);
    T_CHECK(mp_budget_fits(&c, 17520));
    T_CHECK(!mp_budget_fits(&c, 17519));
    T_EQ(MP_BUDGET_CAP_DEFAULT, 16554 + 1024);   /* DaynaPORT + decision 4's allowance */
    T_CHECK(mp_budget_fits(&c, MP_BUDGET_CAP_DEFAULT));
}

static void test_configured_fits_cap(void)
{
    mp_budget_cfg c;
    mp_budget_defaults(&c);
    c.rx_slots = MP_RX_SLOTS;
    c.tx_slots = MP_TX_SLOTS;
    printf("  budget: %u RX + %u TX slots -> %lu bytes, cap %lu\n",
           (unsigned)c.rx_slots, (unsigned)c.tx_slots,
           (unsigned long)mp_budget_total(&c), (unsigned long)MP_MEM_CAP);
    T_CHECK(mp_budget_total(&c) <= (uint32_t)MP_MEM_CAP);
    T_CHECK(c.rx_slots >= 1 && c.rx_slots <= MP_RING_MAX);
    T_CHECK(c.tx_slots >= 1 && c.tx_slots <= MP_RING_MAX);
}

static void test_core_state_fits_allowance(void)
{
    uint32_t s = mp_budget_core_state();
    printf("  core state structs (host): %lu bytes, allowance %u\n",
           (unsigned long)s, (unsigned)MP_BUDGET_STATE_DEFAULT);
    T_CHECK(s <= MP_BUDGET_STATE_DEFAULT);
}

static void test_shrunk_config(void)
{
    mp_budget_cfg c;
    mp_budget_defaults(&c);
    c.rx_slots = 2;
    c.tx_slots = 1;
    T_EQ(mp_budget_total(&c), 9400 + 3 * 1524 + 500);    /* about 14.5 KB, PLAN 2.4 */
}

T_MAIN(test_plan_table, test_configured_fits_cap, test_core_state_fits_allowance,
       test_shrunk_config)

/* mp_budget.c: resident memory arithmetic (boot-time only). See mp_budget.h. */
#include "mp_budget.h"
#include "mp_ring.h"
#include "mp_dev.h"
#include "mp_demux.h"
#include "mp_poll.h"

void mp_budget_defaults(mp_budget_cfg *c)
{
    c->rx_slots = MP_BUDGET_RX_DEFAULT;
    c->tx_slots = MP_BUDGET_TX_DEFAULT;
    c->code_bytes = MP_BUDGET_CODE_DEFAULT;
    c->state_bytes = MP_BUDGET_STATE_DEFAULT;
}

uint32_t mp_budget_total(const mp_budget_cfg *c)
{
    return (uint32_t)c->code_bytes + (uint32_t)c->state_bytes +
           ((uint32_t)c->rx_slots + (uint32_t)c->tx_slots) * MP_SLOT_SIZE;
}

uint32_t mp_budget_core_state(void)
{
    return (uint32_t)(2u * sizeof(mp_ring) + sizeof(mp_rx) + sizeof(mp_tx) +
                      sizeof(mp_demux) + sizeof(mp_poll));
}

int mp_budget_fits(const mp_budget_cfg *c, uint32_t cap)
{
    return mp_budget_total(c) <= cap;
}

/* mp.h: MoofPort portable core, umbrella header.
 * Module map: mp_util (byte access, time, result codes), mp_ring (slot ring),
 * mp_dev (DaynaPORT codec, READ parser, TX), mp_demux (protocol handlers),
 * mp_poll (adaptive poll rate), mp_budget (resident memory). */
#ifndef MP_H
#define MP_H
#include "mp_util.h"
#include "mp_ring.h"
#include "mp_dev.h"
#include "mp_demux.h"
#include "mp_poll.h"
#include "mp_budget.h"
#endif

/* mp_scsi.c - mp_bus over the original SCSI Manager (see mp_scsi.h).
 *
 * Every transaction is SCSIGet, SCSISelect, SCSICmd, data with SCSIRead/SCSIWrite TIBs, and
 * SCSIComplete, the sequence the BlueSCSI Wi-Fi DA uses (docs/dev/RESEARCH.md). Phase 1 moves
 * every byte polled. Phase 3: with MpScsi.blind set (MP_CTL_TUNE), a DATA IN marked MP_XF_BLIND
 * (a READ's payload; its header is always handshaked) goes with SCSIRBlind. Off by default
 * until the verifying probe passes on hardware (PLAN 2.2 gate). With MpScsi.blind_tx set, a
 * DATA OUT marked MP_XF_BLIND (the frame bytes of a WRITE; the 4-byte preamble, padding and
 * trailer stay handshaked) goes with SCSIWBlind; off by default until hw5 checks it.
 * Several SCSIRead calls after one SCSICmd work in Snow and on the Classic (hw1 probe 5B).
 * This runs at task time (EWrite) and in the Time Manager task, so nothing here allocates.
 */
#include <string.h>
#include "scsimgr.h"
#include "mp_scsi.h"

#ifdef MP_DIAG
#define MP_DIAG_DRIVER
#include "mp_diag.h"
#define DIAG_BUS(s, step, cdb) do { if ((s)->dg) mp_diag_bus((MpDiag *)(s)->dg, step, cdb); } while (0)
#else
#define DIAG_BUS(s, step, cdb) do { } while (0)
#endif

#define REQ_WAIT 2000            /* SCSIStat polls while waiting for REQ between packets */

static int b_begin(void *ctx, const uint8_t *cdb)
{
    MpScsi *s = (MpScsi *)ctx;
    OSErr e;
    DIAG_BUS(s, 0, cdb);
    e = SCSIGet();
    if (e == scMgrBusyErr || e == scArbNBErr)
        return MP_E_BUSY;
    if (e)
        return MP_E_BUS;
    if (SCSISelect(s->id) != noErr || SCSICmd((Ptr)cdb, 6) != noErr) {
        short st, msg;
        SCSIComplete(&st, &msg, 1);
        return MP_E_BUS;
    }
    DIAG_BUS(s, 1, cdb);
    return MP_OK;
}

static int xfer(uint8_t *p, uint16_t n, int out, int blind)
{
    SCSIInstr tib[2];
    tib[0].scOpcode = scInc;
    tib[0].scParam1 = (long)p;
    tib[0].scParam2 = n;
    tib[1].scOpcode = scStop;
    tib[1].scParam1 = 0;
    tib[1].scParam2 = 0;
    if (blind)
        return (out ? SCSIWBlind((Ptr)tib) : SCSIRBlind((Ptr)tib)) == noErr ? MP_OK : MP_E_BUS;
    return (out ? SCSIWrite((Ptr)tib) : SCSIRead((Ptr)tib)) == noErr ? MP_OK : MP_E_BUS;
}

/* A discard (MP_XF_NOINC) goes to the core's 16-byte sink in pieces of at most MP_RX_SINK. Not
 * scNoInc: a polled SCSIRead advanced the address anyway (Snow, Phase 1), and a whole frame
 * landed on the driver's state behind the sink. */
static int b_xin(void *ctx, uint8_t *dst, uint16_t n, uint8_t how)
{
    MpScsi *s = (MpScsi *)ctx;
    if (how == MP_XF_NOINC) {
        while (n) {
            uint16_t k = n > MP_RX_SINK ? MP_RX_SINK : n;
            if (xfer(dst, k, 0, 0) != MP_OK)
                return MP_E_BUS;
            n = (uint16_t)(n - k);
        }
        return MP_OK;
    }
    return xfer(dst, n, 0, how == MP_XF_BLIND && s->blind);
}

static int b_xout(void *ctx, const uint8_t *src, uint16_t n, uint8_t how)
{
    MpScsi *s = (MpScsi *)ctx;
    return xfer((uint8_t *)src, n, 1, how == MP_XF_BLIND && s->blind_tx);
}

/* Still in DATA IN? The phase lines are only valid with REQ asserted, and the firmware pauses
 * 300 us between packets [fw], so wait a bounded time for REQ; a target leaving BSY is done. */
static int b_in_data(void *ctx)
{
    int i;
    (void)ctx;
    for (i = 0; i < REQ_WAIT; i++) {
        short st = SCSIStat();
        if (!(st & scsiStatBSY))
            return 0;
        if (st & scsiStatREQ)
            return (st & (scsiStatMSG | scsiStatCD | scsiStatIO)) == scsiStatIO;
    }
    return 0;
}

static int b_end(void *ctx)
{
    short st = 0, msg = 0;
    OSErr e;
    (void)ctx;
    DIAG_BUS((MpScsi *)ctx, 2, 0);
    e = SCSIComplete(&st, &msg, 300);
    DIAG_BUS((MpScsi *)ctx, 3, 0);
    if (e != noErr)
        return MP_E_BUS;
    return st & 0xFF;
}

void mp_scsi_bus(MpScsi *s, mp_bus *bus)
{
    bus->ctx = s;
    bus->begin = b_begin;
    bus->xin = b_xin;
    bus->xout = b_xout;
    bus->in_data = b_in_data;
    bus->end = b_end;
}

int mp_scsi_cmd_in(const mp_bus *bus, const uint8_t cdb[6], uint8_t *buf, uint16_t n)
{
    int rc = bus->begin(bus->ctx, cdb), st;
    if (rc)
        return rc;
    if (n)
        rc = bus->xin(bus->ctx, buf, n, MP_XF_POLLED);
    st = bus->end(bus->ctx);
    if (rc)
        return rc;
    return st == 0 ? MP_OK : (st < 0 ? MP_E_BUS : MP_E_STATUS);
}

short mp_scsi_find(void)
{
    MpScsi s;
    mp_bus bus;
    uint8_t cdb[6], inq[MP_INQUIRY_LEN];
    s.blind = 0;
    s.blind_tx = 0;
#ifdef MP_DIAG
    s.dg = 0;
#endif
    mp_scsi_bus(&s, &bus);
    for (s.id = 0; s.id <= 6; s.id++) {
        memset(inq, 0, sizeof inq);
        mp_cdb_inquiry(cdb, MP_INQUIRY_LEN);
        if (mp_scsi_cmd_in(&bus, cdb, inq, MP_INQUIRY_LEN) == MP_OK &&
            mp_dev_inquiry_match(inq, MP_INQUIRY_LEN))
            return s.id;
    }
    return -1;
}

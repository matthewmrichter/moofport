/* mp_scsi.h - the core's mp_bus on the original SCSI Manager, plus the few whole
 * commands the driver sends outside READ/WRITE (INQUIRY, enable, MAC). */
#ifndef MP_SCSI_H
#define MP_SCSI_H

#include "mp_dev.h"

typedef struct MpScsi {
    short id;                 /* the DaynaPORT's SCSI ID */
    unsigned char blind;      /* 1 = MP_XF_BLIND DATA IN uses SCSIRBlind (PLAN 2.2 gate) */
    unsigned char blind_tx;   /* 1 = MP_XF_BLIND DATA OUT uses SCSIWBlind (after hw4) */
#ifdef MP_DIAG
    void *dg;                 /* the hwsend diagnostic build's MpDiag: WRITE phase stamps */
#endif
} MpScsi;

/* Fill `bus` with the SCSI Manager callbacks for target `s`. */
void mp_scsi_bus(MpScsi *s, mp_bus *bus);

/* One command with an optional DATA IN of n bytes (n 0 = none). MP_OK, MP_E_BUSY,
 * MP_E_BUS or MP_E_STATUS. */
int mp_scsi_cmd_in(const mp_bus *bus, const uint8_t cdb[6], uint8_t *buf, uint16_t n);

/* INQUIRY targets 0..6 for a DaynaPORT ("Dayna", "SCSI/Link"); its ID or -1. */
short mp_scsi_find(void);

#endif

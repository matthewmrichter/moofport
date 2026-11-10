/* fakedev.h: host model of the BlueSCSI DaynaPORT target, behind an mp_bus.
 *
 * Three personalities, each following its source line by line:
 *   FD_FW_OLD   BlueSCSI-v2 network.c before e7d9629 (firmware before 2026-01):
 *               one packet per READ (len < 64 reported as 64, len + 6 > alloc
 *               truncated to alloc - 6), flags 0x10 if more is queued; WRITE with
 *               CDB[5] == 0x80 reads exactly CDB len + 8 bytes, else CDB len.
 *               Enqueue does not pad; CRC appended little-endian.
 *   FD_FW_2608  network.c at v2026.08.17 / 5e64e3b: multi-packet READ only with
 *               CDB[5] bit 6, stopping when total + 1530 > alloc or total >= 3060;
 *               75 us pause after each header, 300 us between packets; WRITE
 *               with CDB[5] != 0 reads [4-byte header][len] until len 0 (len
 *               capped at 1520). Enqueue pads to 60, CRC little-endian.
 *   FD_SNOW     snow core/src/mac/scsi/ethernet.rs: up to 8 packets per READ
 *               regardless of CDB[5], no pauses, frame padded to 64 + 4-byte
 *               big-endian CRC, CHECK CONDITION (and the packets already
 *               dequeued lost) if one packet's 6 + len exceeds alloc; WRITE with
 *               CDB[5] bit 7 = header, then (packet + next header) until 0.
 * Both firmware queues are the firmware's 20-slot ring, including its overflow
 * behaviour: when the write index catches the read index the queue reads as
 * empty (network.c scsiNetworkEnqueue). The Snow queue is a FIFO capped at
 * FD_QCAP here (512 in Snow).
 *
 * The model records where the device pauses in a DATA IN stream and counts
 * blind transfers that run across a pause (the Classic risk PLAN 2.2 splits
 * transfers for).
 */
#ifndef FAKEDEV_H
#define FAKEDEV_H

#include <stdint.h>
#include "mp_dev.h"

enum { FD_FW_OLD = 0, FD_FW_2608 = 1, FD_SNOW = 2 };
enum { FD_FREE = 0, FD_DATA_IN, FD_DATA_OUT, FD_STATUS };

#define FD_FW_QUEUE   20u        /* NETWORK_PACKET_QUEUE_SIZE */
#define FD_QCAP       64u
#define FD_PKT        1520u      /* NETWORK_PACKET_MAX_SIZE */
#define FD_IN_MAX     40000u
#define FD_GAPS_MAX   128u
#define FD_SENT_MAX   64u

typedef struct fakedev {
    int kind;
    uint8_t mac[6];
    int enabled;

    /* network -> Mac queue */
    uint8_t q[FD_QCAP][FD_PKT];
    uint16_t qlen[FD_QCAP];
    uint16_t rd, wr, qn;          /* fw: ring indices; snow: FIFO head + count */
    uint32_t net_dropped;         /* refused at enqueue */

    /* current transaction */
    int phase;
    uint8_t cdb[6];
    int status;

    uint8_t in[FD_IN_MAX];
    uint32_t in_len, in_pos;
    uint32_t gaps[FD_GAPS_MAX];   /* stream offsets the device pauses before */
    uint16_t ngaps;

    int wstage;                   /* write state machine */
    uint8_t wbuf[FD_PKT + 16];
    uint32_t wwant, wgot;
    uint16_t wsize;               /* CDB[3..4] */

    /* Mac -> network */
    uint8_t sent[FD_SENT_MAX][FD_PKT];
    uint16_t sent_len[FD_SENT_MAX];
    uint16_t nsent;
    uint8_t mcast[8][6];
    uint16_t nmcast;
    int wifi_joined;
    char wifi_ssid[65];

    /* injection and counters */
    int busy_next;                /* begin() returns MP_E_BUSY this many times */
    uint32_t fail_in_after;       /* nonzero: DATA IN fails after this many bytes */
    uint32_t cmds, reads, writes, checks;
    uint32_t xfer_polled, xfer_blind, xfer_noinc;   /* bytes by mode */
    uint32_t blind_gap_cross;     /* blind DATA IN transfers that ran across a pause */
    uint32_t blind_gap_start;     /* blind transfers that began right after a pause */
    uint32_t abandoned;           /* end() called while still in a data phase */
} fakedev;

void fd_init(fakedev *d, int kind);
/* Bind an mp_bus to the model. */
void fd_bus(fakedev *d, mp_bus *bus);
/* A frame arrives from the network. Returns 1 queued, 0 refused. */
int fd_net_rx(fakedev *d, const uint8_t *frame, uint16_t len);
/* Packets currently readable. */
uint16_t fd_queued(const fakedev *d);
uint32_t fd_crc32(const uint8_t *p, uint32_t n);

#endif

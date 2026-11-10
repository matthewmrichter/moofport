/* mp_drv.h - what the MoofPort driver shares with its diagnostic tools: the private Status
 * call that returns the driver's counters, and the private Control call that tunes it. */
#ifndef MP_DRV_H
#define MP_DRV_H

#include "mp_dev.h"

#define MP_STATUS_STATS  0x4D50      /* 'MP' */
#define MP_STATS_VERSION 8

/* Private Control csCode MP_CTL_TUNE ('MT'): ePointer = an MpTune. Sets the poll timing
 * (mp_poll_cfg, ms), blind payload reads and checksum verification, until the next restart.
 * The current values come back in MpStats.tune. */
#define MP_CTL_TUNE      0x4D54
#define MP_TUNE_VERSION  5
#define MP_TUNE_NOBATCH  0x01        /* flags: one frame per WRITE (no multi-frame WRITEs) */
#define MP_TUNE_RX4      0x02        /* flags: up to 4 received frames per poll tick (else 2; hwsend6) */
#define MP_TUNE_ACK2     0x04        /* flags: poll 2 ms after a WRITE (else 5; hwsend5) */
#define MP_TUNE_NOCHASE  0x08        /* flags: no reply chase after a WRITE (one early poll only) */
#define MP_TUNE_NOYIELD  0x10        /* flags: keep reading after a READ that brought only replies */
#define MP_TUNE_NOINLINE 0x20        /* flags: EWrite queues every frame (else it writes at once when
                                        nothing is queued: the default since hwsend9) */
#define MP_TUNE_HUSH     0x40        /* flags: no poll for 15 ms after handing MacTCP only small TCP
                                        frames, unless it sends (hwsend8; 0x80 was 10/20 ms until
                                        hwsend11) */

typedef struct MpTune {
    uint16_t version;
    uint16_t burst_ms, hold_ms, idle_ms, step_ms, busy_ms;
    uint8_t  blind;           /* 1 = READ payloads with SCSIRBlind (header always handshaked) */
    uint8_t  verify;          /* 1 = check IPv4 header and TCP/UDP checksums of every frame */
    /* version 2 (after hw4) */
    uint8_t  blind_tx;        /* 1 = WRITE frame bytes with SCSIWBlind (preamble handshaked) */
    uint8_t  flags;           /* MP_TUNE_* below (send-fix; before: padding) */
    /* version 3 (1.0): mp_poll's middle level, see core/mp_poll.h */
    uint16_t mid_ms, quiet_ms;
    /* version 4 (hwsend9) */
    uint8_t  early;           /* MP_EARLY_*: the first READ after an inline WRITE */
    uint8_t  pad;             /* diagnostic driver only: ms to spin after an inline WRITE (Snow) */
    /* version 5 (hwsend11, the poll-path design) */
    uint8_t  flags2;          /* MP_TUNE2_* below */
    uint8_t  pad2;
} MpTune;

#define MP_TUNE2_NOPAUSE 0x01        /* no 12 ms pause after the reply to our own WRITE */
#define MP_TUNE2_REPOLL  0x02        /* re-poll 1 ms after a READ cut short by the yield (hwsend10) */
#define MP_TUNE2_STEADY  0x04        /* one frame per READ, no early poll after an inline WRITE (with
                                        MoofTune steady: DaynaPORT's rhythm, hwsend12) */

#define MP_EARLY_2       0           /* chase2: a poll during the WRITE, then every 2 ms (hwsend10) */
#define MP_EARLY_1       1           /* chase1: the same every 1 ms */
#define MP_EARLY_OFF     2           /* the first poll ack_ms after the WRITE, then the chase at
                                        ack_ms (the default since hwsend11: the ACK is back 3-5 ms
                                        after the WRITE, so earlier READs come back empty) */

typedef struct MpStats {
    uint16_t version;
    int16_t  scsi_id;
    uint8_t  opened;
    uint8_t  mac[6];
    uint8_t  pad;
    uint32_t polls, busy, delivered, tx_now, tx_deferred, tx_failed, bus_err;
    int16_t  last_err;
    int16_t  pad2;
    mp_rx_stats rx;
    mp_tx_stats tx;
    uint32_t unattached, runts;
    /* version 3 (Phase 3) */
    MpTune   tune;
    uint32_t kicks;           /* EWrites that pulled the next poll forward */
    uint32_t bursts, immediates;   /* mp_poll: polls scheduled at the burst rate / at once */
    uint32_t v_ok, v_bad;     /* verify: IPv4 frames whose checksums were right / wrong */
    uint32_t v_big;           /* ... of the right ones, TCP/UDP frames of 1,000 bytes or more */
    /* version 7 (hwsend10) */
    uint32_t wd;              /* the watchdog found the poll task stuck and re-armed it */
} MpStats;

/* Private Status csCode MP_STATUS_LOG ('ML'): Control call counts and the handler table, for
 * finding out what a client (AppleTalk, MacTCP) asks of the driver. Version 2 (Phase 3) drops
 * version 1's log of the first 24 calls (336 resident bytes; Phase 2 bring-up is done). */
#define MP_STATUS_LOG  0x4D4C
#define MP_LOG_VERSION 2

typedef struct MpCalls {
    uint32_t calls[10];       /* Control calls by csCode 245..253 (245 EDelMulti, 246 EAddMulti), [9] other */
    uint32_t write8023;       /* EWrites whose type/length field is a length (802.3) */
} MpCalls;

typedef struct MpLog {
    uint16_t version;
    uint16_t pad;
    MpCalls  c;
    struct { uint16_t type; uint8_t used, has_handler; uint32_t frames; } ph[8];
} MpLog;

#endif

/* mp_dev.h: the BlueSCSI v2 / DaynaPORT SCSI protocol codec.
 *
 * Sources (cited as [fw] and [snow] below):
 *   [fw]   BlueSCSI-v2 lib/SCSI2SD/src/firmware/network.c + network.h, commit
 *          5e64e3b (2026-10-03); same READ/WRITE code as tag v2026.08.17.
 *          "Old firmware" = network.c before e7d9629 (2026-01-12).
 *   [snow] twvd/snow core/src/mac/scsi/ethernet.rs, commit fbdcbac.
 *
 * Three layers, all portable:
 *   1. CDB builders and field codecs (no state).
 *   2. mp_rx: an incremental READ-response parser. The Mac glue asks it where
 *      the next bytes go and how many (mp_rx_want), moves them with whatever
 *      SCSI Manager call fits (polled SCSIRead for the 6-byte header, blind
 *      SCSIRBlind straight into the RX slot for the payload), then reports them
 *      (mp_rx_got). After each packet the glue checks the bus phase: still
 *      DATA IN means another header follows (mp_rx_next); anything else ends the
 *      transaction (mp_rx_end). That works on old firmware, new firmware and
 *      Snow without a version check (PLAN 2.2).
 *   3. mp_bus + mp_rx_poll / mp_tx_*: a complete transaction on top of a small
 *      table of bus callbacks, so the whole poll/flush logic is host-tested
 *      against tests/fakedev and the Mac glue only supplies SCSI Manager
 *      wrappers.
 */
#ifndef MP_DEV_H
#define MP_DEV_H

#include "mp_util.h"
#include "mp_ring.h"

/* ---- Constants ---------------------------------------------------------- */

#define MP_ETH_HDR       14u
#define MP_ETH_MIN       60u     /* minimum frame without FCS; TX pads to this */
#define MP_ETH_MAX       1514u   /* maximum frame without FCS */
#define MP_DEV_CRC       4u      /* trailing CRC bytes counted in an RX len [fw] */
#define MP_DEV_RX_HDR    6u      /* len_hi len_lo 0 0 0 flags [fw] */
#define MP_DEV_TX_HDR    4u      /* len_hi len_lo 0 0 [fw] */
#define MP_DEV_PKT_MAX   1520u   /* NETWORK_PACKET_MAX_SIZE: largest RX len [fw] */
#define MP_DEV_READ_UNIT 1530u   /* DAYNAPORT_SCSI_PACKET_MAX (1524) + 6 [fw] */
#define MP_DEV_RX_MIN    64u     /* smallest RX len sent: 60 + CRC [fw]; old fw and Snow use 64/68 */
#define MP_DEV_RX_MORE   0x10u   /* header flags: another packet is queued [fw] */

/* SCSI opcodes [fw scsiNetworkCommand] */
#define MP_OP_TUR        0x00u
#define MP_OP_READ       0x08u
#define MP_OP_STATS      0x09u   /* MAC + 3 counters, 18 bytes */
#define MP_OP_WRITE      0x0Au
#define MP_OP_SETMODE    0x0Cu   /* ignored by the firmware */
#define MP_OP_MCAST      0x0Du   /* add multicast address, 6 bytes DATA OUT */
#define MP_OP_ENABLE     0x0Eu   /* CDB[5] bit 7 = enable */
#define MP_OP_INQUIRY    0x12u
#define MP_OP_WIFI       0x1Cu   /* BlueSCSI Wi-Fi, subcommand in CDB[1] */

/* READ(6) CDB[5] [fw, commit 7c20579] */
#define MP_READ_SINGLE   0x80u   /* bit 6 clear: one packet per READ */
#define MP_READ_MULTI    0xC0u   /* bit 6 set: several packets per READ */
/* WRITE(6) CDB[5] [fw] */
#define MP_WRITE_RAW      0x00u  /* one packet of CDB[3..4] bytes, no header */
#define MP_WRITE_PREAMBLE 0x80u  /* [4-byte header][frame]..., ended by len 0 */

/* 0x1C subcommands [fw network.h] */
#define MP_WIFI_SCAN      0x01u
#define MP_WIFI_COMPLETE  0x02u
#define MP_WIFI_RESULTS   0x03u
#define MP_WIFI_INFO      0x04u
#define MP_WIFI_JOIN      0x05u
#define MP_WIFI_ENTRY_LEN 74u    /* struct wifi_network_entry, packed [fw network.h] */
#define MP_WIFI_JOIN_LEN  130u   /* struct wifi_join_request, packed [fw network.h] */
#define MP_WIFI_SSID_MAX  64u
#define MP_WIFI_KEY_MAX   64u

#define MP_STATS_LEN      18u    /* 6-byte MAC + three uint32 counters (always 0) [fw] */
#define MP_INQUIRY_LEN    36u

/* ---- 1. CDB builders and field codecs ---------------------------------- */

void mp_cdb_inquiry(uint8_t cdb[6], uint8_t alloc);
/* READ(6): alloc in CDB[3..4], mode MP_READ_SINGLE or MP_READ_MULTI in CDB[5].
 * The firmware answers alloc == 1 with CHECK CONDITION [fw, snow]. */
void mp_cdb_read(uint8_t cdb[6], uint16_t alloc, uint8_t mode);
/* WRITE(6) raw: one frame of 'len' bytes, no header (CDB[5] = 0x00). */
void mp_cdb_write_raw(uint8_t cdb[6], uint16_t len);
/* WRITE(6) preamble: CDB[5] = 0x80, CDB[3..4] = first frame's length.
 * Old firmware then reads exactly len + 8 bytes; new firmware and Snow ignore
 * CDB[3..4] and read headers until a zero length [fw before/after e7d9629, snow]. */
void mp_cdb_write_preamble(uint8_t cdb[6], uint16_t first_len);
void mp_cdb_stats(uint8_t cdb[6]);
void mp_cdb_mcast(uint8_t cdb[6]);
void mp_cdb_enable(uint8_t cdb[6], int on);
void mp_cdb_wifi(uint8_t cdb[6], uint8_t sub, uint16_t len);

/* INQUIRY data: vendor "Dayna" (bytes 8..15) and product "SCSI/Link"
 * (bytes 16..31), space padded [fw BlueSCSI_config.h DRIVEINFO_NETWORK, snow].
 * Returns 1 on a match. */
int mp_dev_inquiry_match(const uint8_t *inq, uint16_t n);

/* READ allocation length for 'free_slots' free RX slots.
 * safe = 1: the firmware's multi-packet loop stops before packet k+1 when
 *   total + 1530 > alloc, and every packet costs at least 70 bytes (64 + 6),
 *   so alloc = 70*(k-1) + 1530 never yields more than k packets [fw].
 *   (Full-size frames then come one per READ unless k is large.)
 * safe = 0: k * 1530 (more full frames per READ; small ones may overflow the
 *   ring and are counted as rx overflow).
 * Snow ignores alloc beyond a per-packet check and sends up to 8 [snow].
 * Result is at least 1530 (old firmware truncates a packet to alloc - 6). */
uint16_t mp_dev_read_alloc(uint16_t free_slots, int safe);

/* 4-byte TX preamble header. len 0 = end of a batch. */
void mp_dev_tx_hdr(uint8_t out[4], uint16_t len);
/* Padded TX length: max(len, 60). */
uint16_t mp_dev_tx_padlen(uint16_t len);

/* 0x09 reply: MAC + three counters. Returns MP_OK or MP_E_LEN. ctr may be NULL. */
int mp_dev_parse_stats(const uint8_t *r, uint16_t n, uint8_t mac[6], uint32_t ctr[3]);

typedef struct mp_wifi_entry {
    char ssid[MP_WIFI_SSID_MAX + 1];   /* NUL-terminated */
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t flags;                     /* bit 0 = auth required [fw] */
} mp_wifi_entry;
#define MP_WIFI_FLAG_AUTH 0x01u

/* One 74-byte wifi_network_entry. */
void mp_dev_wifi_entry(const uint8_t *p, mp_wifi_entry *e);
/* RESULTS (3) / INFO (4) reply: [len_hi len_lo][entries]. Returns the number of
 * whole entries present (bounded by n), *first set to the first entry. */
uint16_t mp_dev_wifi_list(const uint8_t *r, uint16_t n, const uint8_t **first);
/* JOIN (5) DATA OUT: exactly 130 bytes [fw scsiNetworkWifiJoin]. Strings are
 * truncated to 63 chars + NUL. */
void mp_dev_wifi_join(uint8_t out[MP_WIFI_JOIN_LEN], const char *ssid,
                      const char *key, uint8_t channel);

/* ---- 3a. Bus callbacks --------------------------------------------------- */

/* Transfer hints. The glue maps them to SCSI Manager TIB opcodes. */
#define MP_XF_POLLED 0u   /* handshake every byte (SCSIRead/SCSIWrite) */
#define MP_XF_BLIND  1u   /* blind transfer (SCSIRBlind/SCSIWBlind) */
#define MP_XF_NOINC  2u   /* discard: dst is a 16-byte sink, may use scNoInc */

typedef struct mp_bus {
    void *ctx;
    /* Claim the bus, select the target, send the 6-byte CDB. 0 = command sent,
     * MP_E_BUSY = bus or SCSI Manager busy (nothing happened), else MP_E_BUS. */
    int (*begin)(void *ctx, const uint8_t *cdb);
    /* Move n bytes in DATA IN / DATA OUT. 0 = all n moved, else MP_E_BUS
     * (phase changed early, timeout). */
    int (*xin)(void *ctx, uint8_t *dst, uint16_t n, uint8_t how);
    int (*xout)(void *ctx, const uint8_t *src, uint16_t n, uint8_t how);
    /* 1 while the target is in a data phase (DATA IN for reads), else 0. */
    int (*in_data)(void *ctx);
    /* Finish (status + message). Returns the SCSI status byte (0 = GOOD) or a
     * negative MP_E_* code. Always called once after a successful begin. */
    int (*end)(void *ctx);
} mp_bus;

/* ---- 2. READ-response parser -------------------------------------------- */

typedef struct mp_rx_stats {
    uint32_t polls;       /* READ transactions started */
    uint32_t empty;       /* READs that returned the 6-byte empty header */
    uint32_t frames;      /* frames committed to the ring */
    uint32_t bad;         /* header len below 18 (CRC + Ethernet header): skipped */
    uint32_t oversize;    /* header len above 1520: skipped */
    uint32_t overflow;    /* no free RX slot: frame skipped (dequeued by the device) */
    uint32_t aborted;     /* transactions abandoned on a bus/status error */
    uint32_t more_ended;  /* READ ended after a header with flags 0x10 */
    uint32_t multi;       /* READs that carried 2 or more packets */
    uint32_t drained;     /* ring full mid-READ: on_full emptied it (mp_rx_set_full) */
} mp_rx_stats;

/* Parser states (mp_rx_want's return value). */
#define MP_RX_IDLE    0u   /* no transaction */
#define MP_RX_HDR     1u   /* wants header bytes */
#define MP_RX_DATA    2u   /* wants payload bytes into the RX slot */
#define MP_RX_SKIP    3u   /* wants payload bytes to discard (sink, may use NOINC) */
#define MP_RX_PKT     4u   /* a packet finished: check the phase, then next or end */
#define MP_RX_EMPTY   5u   /* empty header: the target goes to STATUS next */

#define MP_RX_SINK 16u

typedef struct mp_rx {
    mp_ring *ring;
    uint8_t *dst;          /* where the next bytes go */
    uint16_t need;         /* bytes still wanted for the current piece */
    uint16_t len;          /* current packet's len field */
    uint8_t state;
    uint8_t flags;         /* last header's flags byte */
    uint8_t npkt;          /* packets in this transaction */
    uint8_t saw_multi;     /* 1 once any READ carried 2+ packets (new fw or Snow) */
    uint8_t hdr[MP_DEV_RX_HDR];
    uint8_t sink[MP_RX_SINK];
    void (*on_full)(void *ctx);   /* see mp_rx_set_full */
    void *full_ctx;
    mp_rx_stats st;
} mp_rx;

void mp_rx_init(mp_rx *p, mp_ring *ring);
/* Optional: called when a packet arrives and the ring has no free slot, before the packet is
 * skipped. It runs in the middle of the READ (the bus is still held) and may consume ring
 * slots, typically by delivering frames to their handlers; a handler's EWrite then finds the
 * bus busy and is deferred. Snow sends up to 8 packets per READ whatever alloc says, so without
 * this a small ring drops frames there. NULL = skip and count overflow, as before. */
void mp_rx_set_full(mp_rx *p, void (*fn)(void *ctx), void *ctx);
/* A READ CDB was sent: expect a header. */
void mp_rx_begin(mp_rx *p);
/* Where and how many bytes next. Returns the state; *dst / *n are set for
 * MP_RX_HDR, MP_RX_DATA and MP_RX_SKIP (for SKIP, *dst is the 16-byte sink and
 * *n the whole remainder: move at most MP_RX_SINK per call, or use NOINC). */
uint8_t mp_rx_want(const mp_rx *p, uint8_t **dst, uint16_t *n);
/* n bytes (1..*n of mp_rx_want) have been moved. Returns the new state. A
 * frame is committed to the ring when its last byte arrives. */
uint8_t mp_rx_got(mp_rx *p, uint16_t n);
/* After MP_RX_PKT with the target still in DATA IN: another header follows. */
void mp_rx_next(mp_rx *p);
/* The target left DATA IN. Returns 1 if the device said more is queued (the
 * last header had flags 0x10 but no further packet came in this READ), so poll
 * again now. Only old firmware does this: v2026.08.17 and Snow clear the flag
 * on the last packet of a READ even when they stop at a cap with more queued
 * [fw: flags = done ? 0 : 0x10; snow: more = !empty && count < 8], so after
 * any RX the poll rate must burst anyway (mp_poll does). */
int mp_rx_end(mp_rx *p);
/* Bus error mid-transaction: drop any partly read packet. */
void mp_rx_abort(mp_rx *p);

/* One full READ transaction on 'bus'. 'lead' = payload bytes read polled along
 * with the header before switching to blind (0 = header only polled). Returns
 * the frames committed (>= 0) or MP_E_BUSY / MP_E_BUS / MP_E_STATUS. *more is
 * set as by mp_rx_end (0 on error). Bounded: at most 255 packets. */
int mp_rx_poll(mp_rx *p, const mp_bus *bus, uint8_t mode, uint16_t alloc,
               uint16_t lead, int *more);

/* ---- 3b. Transmit -------------------------------------------------------- */

typedef struct mp_seg {
    uint8_t *p;           /* non-const: the source MAC is written at +6 of the frame */
    uint16_t len;
} mp_seg;

typedef struct mp_tx_stats {
    uint32_t sent;        /* frames handed to the device */
    uint32_t queued;      /* frames copied into the TX deferral ring */
    uint32_t full;        /* refused: TX ring full */
    uint32_t badlen;      /* refused: frame shorter than 14 or longer than 1514 */
    uint32_t writes;      /* WRITE transactions */
    uint32_t batches;     /* WRITEs carrying 2+ frames */
    uint32_t errors;      /* WRITEs that failed (bus/status) */
    uint32_t batch_off;   /* batching turned off after a failed batch */
} mp_tx_stats;

#define MP_TX_BATCH_MAX 8u

typedef struct mp_tx {
    mp_ring *q;            /* deferral queue (frames stored padded, source MAC set) */
    uint8_t mac[6];
    uint8_t batch;         /* 1 = multi-frame preamble WRITE allowed (fw v2026.01+) */
    uint8_t max_batch;     /* frames per batched WRITE, 1..MP_TX_BATCH_MAX */
    mp_tx_stats st;
} mp_tx;

void mp_tx_init(mp_tx *t, mp_ring *q, const uint8_t mac[6], int batch);
/* Total length of a segment list. */
uint32_t mp_seg_total(const mp_seg *s, uint16_t n);
/* Copy a frame (gather) into the deferral ring: source MAC at +6, zero pad to 60.
 * MP_OK, MP_E_FULL, MP_E_LEN. Producer side of t->q. */
int mp_tx_queue(mp_tx *t, const mp_seg *s, uint16_t n);
/* Send one frame now, zero copy, as the universal single-frame preamble WRITE:
 * [len 0 0][frame][pad to 60][00 00 00 00] with CDB[3..4] = padded len. Works
 * on old and new firmware and Snow [fw, snow]. Writes our MAC at +6 of the
 * frame (s[0] must be at least 12 bytes, else MP_E_LEN: queue it instead).
 * MP_OK, MP_E_BUSY (nothing sent: queue it), MP_E_BUS, MP_E_STATUS, MP_E_LEN. */
int mp_tx_send(mp_tx *t, const mp_bus *bus, mp_seg *s, uint16_t n);
/* Flush the deferral ring: one batched WRITE (t->batch) of up to max_batch
 * frames, or one universal single-frame WRITE. Consumer side of t->q. Returns
 * frames sent (0 if the ring is empty) or MP_E_*. On a failed batch, frames
 * whose bytes were all accepted are dropped from the ring (they were sent),
 * the rest stay queued and batching is turned off. */
int mp_tx_flush(mp_tx *t, const mp_bus *bus);

#endif

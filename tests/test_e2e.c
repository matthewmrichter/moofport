/* test_e2e.c: codec + ring + demux driven end to end against the fake device
 * in all three personalities (old firmware, v2026.08.17, Snow). */
#include "t.h"
#include "mp.h"
#include "fakedev.h"

static const int kinds[3] = { FD_FW_OLD, FD_FW_2608, FD_SNOW };
static const char *const kname[3] = { "fw-old", "fw-2608", "snow" };

static fakedev dev;
static mp_bus bus;
static uint8_t rxmem[MP_RING_BYTES(8)];
static uint8_t txmem[MP_RING_BYTES(4)];
static mp_ring rxq, txq;
static mp_rx rx;
static mp_tx tx;
static mp_demux dm;
static int h_ip, h_arp;

/* delivered frames */
#define DMAX 64
static uint16_t dlen[DMAX];
static uint8_t dfirst[DMAX];   /* payload byte 14 = sequence number */
static int dn;
static int echo;               /* handler replies to each IP frame (nested TX) */

static int cmd_in(const uint8_t *cdb, uint8_t *buf, uint16_t n)
{
    int rc = bus.begin(bus.ctx, cdb);
    if (rc)
        return rc;
    if (n)
        rc = bus.xin(bus.ctx, buf, n, MP_XF_POLLED);
    if (bus.end(bus.ctx) != 0)
        return MP_E_STATUS;
    return rc;
}

static int cmd_none(const uint8_t *cdb)
{
    return cmd_in(cdb, NULL, 0);
}

static void setup(int kind, uint16_t rxslots, int batch)
{
    uint8_t cdb[6], st[18], mac[6];
    fd_init(&dev, kind);
    fd_bus(&dev, &bus);
    mp_cdb_enable(cdb, 1);
    cmd_none(cdb);
    mp_cdb_stats(cdb);
    cmd_in(cdb, st, 18);
    mp_dev_parse_stats(st, 18, mac, NULL);
    mp_ring_init(&rxq, rxmem, rxslots);
    mp_ring_init(&txq, txmem, 4);
    mp_rx_init(&rx, &rxq);
    mp_tx_init(&tx, &txq, mac, batch);
    mp_demux_init(&dm);
    mp_demux_attach(&dm, 0x0800, &h_ip);
    mp_demux_attach(&dm, 0x0806, &h_arp);
    dn = 0;
    echo = 0;
}

static void make_frame(uint8_t *f, uint16_t len, uint16_t type, uint8_t seq)
{
    uint16_t i;
    memset(f, 0xFF, 6);
    f[6] = 0x02; f[7] = 0; f[8] = 0; f[9] = 0; f[10] = 0; f[11] = 1;
    f[12] = (uint8_t)(type >> 8);
    f[13] = (uint8_t)type;
    for (i = 14; i < len; i++)
        f[i] = (uint8_t)(seq + i - 14);
}

static void net_rx(uint16_t len, uint8_t seq)
{
    static uint8_t f[1514];
    make_frame(f, len, 0x0800, seq);
    fd_net_rx(&dev, f, len);
}

static void deliver(void)
{
    const uint8_t *f;
    uint16_t len;
    while ((f = mp_ring_peek(&rxq, &len)) != NULL) {
        mp_ph *e = mp_demux_lookup(&dm, f, len);
        if (e && dn < DMAX) {
            dlen[dn] = len;
            dfirst[dn] = f[14];
            dn++;
            if (echo && e->handler == &h_ip) {
                /* handler sends from inside delivery: the bus is free */
                static uint8_t r[64];
                mp_seg s;
                make_frame(r, 64, 0x0800, f[14]);
                s.p = r;
                s.len = 64;
                if (mp_tx_send(&tx, &bus, &s, 1) != MP_OK)
                    mp_tx_queue(&tx, &s, 1);
            }
        }
        mp_ring_pop(&rxq);
    }
}

static int poll_once(uint8_t mode, uint16_t lead, int safe, int *more)
{
    uint16_t alloc = mp_dev_read_alloc(mp_ring_free(&rxq), safe);
    int r = mp_rx_poll(&rx, &bus, mode, alloc, lead, more);
    deliver();
    return r;
}

/* Poll until a READ returns nothing and the device says nothing more. */
static int drain(uint8_t mode, uint16_t lead, int safe)
{
    int total = 0, i, more = 0, r;
    for (i = 0; i < 200; i++) {
        r = poll_once(mode, lead, safe, &more);
        if (r < 0)
            return r;
        total += r;
        if (r == 0 && !more)
            break;
    }
    return total;
}

static void test_empty_polls(void)
{
    int k, more;
    for (k = 0; k < 3; k++) {
        setup(kinds[k], 4, 0);
        T_EQ(poll_once(MP_READ_MULTI, 0, 1, &more), 0);
        T_EQ(poll_once(MP_READ_SINGLE, 0, 1, &more), 0);
        T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), 0);
        T_EQ(more, 0);
        T_EQ(rx.st.empty, 3);
        T_EQ(rx.st.polls, 3);
        T_EQ(dev.abandoned, 0);
        T_EQ(dev.xfer_blind, 0);   /* the empty header is read polled */
    }
}

static void test_rx_sizes(void)
{
    static const uint16_t lens[5] = { 42, 59, 60, 61, 1514 };
    int k, i;
    for (k = 0; k < 3; k++) {
        setup(kinds[k], 8, 0);
        for (i = 0; i < 5; i++)
            net_rx(lens[i], (uint8_t)(i * 10));
        T_EQ(drain(MP_READ_MULTI, 8, 1), 5);
        T_EQ(dn, 5);
        for (i = 0; i < 5; i++) {
            uint16_t want = lens[i];
            /* new fw pads to 60 before the CRC; old fw reports len < 64 as 64
             * (so 60 frame bytes); Snow pads the frame to 64 [fw, snow] */
            if (kinds[k] == FD_SNOW && want < 64)
                want = 64;
            else if (want < 60)
                want = 60;
            if (dlen[i] != want)
                printf("  %s frame %d: len %u want %u\n", kname[k], i, dlen[i], want);
            T_EQ(dlen[i], want);
            T_EQ(dfirst[i], i * 10);
        }
        T_EQ(rx.st.frames, 5);
        T_EQ(rx.st.bad + rx.st.oversize + rx.st.overflow, 0);
    }
}

static void test_max_frame_content(void)
{
    static uint8_t f[1514];
    int k;
    for (k = 0; k < 3; k++) {
        const uint8_t *p;
        uint16_t len;
        setup(kinds[k], 2, 0);
        make_frame(f, 1514, 0x0806, 0x33);
        fd_net_rx(&dev, f, 1514);
        T_EQ(poll_once(MP_READ_MULTI, 8, 1, NULL), 1);
        /* deliver() popped it; read the slot bytes back for a content check */
        p = (const uint8_t *)rxmem + MP_SLOT_HDR;
        len = mp_rd16(rxmem);
        T_EQ(len, 1514);
        T_MEM(p, f, 1514);
        T_EQ(dm.e[1].frames, 1);   /* ARP handler */
    }
}

static void test_multi_packet(void)
{
    int k, i;
    for (k = 0; k < 3; k++) {
        int more = 0, r;
        setup(kinds[k], 8, 0);
        for (i = 0; i < 6; i++)
            net_rx(60, (uint8_t)i);
        r = poll_once(MP_READ_MULTI, 8, 1, &more);
        if (kinds[k] == FD_FW_OLD) {
            T_EQ(r, 1);            /* one per READ ... */
            T_EQ(more, 1);         /* ... but flags 0x10: poll again now */
            T_EQ(rx.saw_multi, 0);
        } else {
            T_EQ(r, 6);            /* all six in one READ */
            T_EQ(more, 0);
            T_EQ(rx.saw_multi, 1); /* evidence that batched WRITE is supported */
        }
        drain(MP_READ_MULTI, 8, 1);
        T_EQ(dn, 6);
        for (i = 0; i < 6; i++)
            T_EQ(dfirst[i], i);    /* order kept */
    }
}

static void test_single_mode_gate(void)
{
    int more = 0;
    setup(FD_FW_2608, 8, 0);
    net_rx(100, 1);
    net_rx(100, 2);
    T_EQ(poll_once(MP_READ_SINGLE, 0, 1, &more), 1);   /* bit 6 clear: one [fw 7c20579] */
    /* v2026.08.17 writes flags = done ? 0 : 0x10 after its stop checks, so a
     * single-mode READ never says more is queued (old fw does) [fw] */
    T_EQ(more, 0);
    T_EQ(rx.st.more_ended, 0);
    T_EQ(poll_once(MP_READ_SINGLE, 0, 1, &more), 1);
    T_EQ(more, 0);
    /* Snow ignores the gate and sends several [snow] */
    setup(FD_SNOW, 8, 0);
    net_rx(100, 1);
    net_rx(100, 2);
    T_EQ(poll_once(MP_READ_SINGLE, 0, 1, &more), 2);
}

static void test_fw_read_cap(void)
{
    /* full frames: the firmware stops once total >= 3060 or alloc is reached */
    int i, more = 0;
    setup(FD_FW_2608, 8, 0);
    for (i = 0; i < 5; i++)
        net_rx(1514, (uint8_t)i);
    T_EQ(poll_once(MP_READ_MULTI, 8, 0, &more), 3);   /* 3 x 1524 = 4572 >= 3060 */
    T_EQ(more, 0);   /* the cap clears the 0x10 flag: the hint is lost [fw] */
    T_EQ(drain(MP_READ_MULTI, 8, 0), 2);
    T_EQ(dn, 5);
    /* safe alloc with 8 free slots = 2020: one full frame per READ */
    setup(FD_FW_2608, 8, 0);
    for (i = 0; i < 3; i++)
        net_rx(1514, (uint8_t)i);
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), 1);
    T_EQ(more, 0);
    T_EQ(fd_queued(&dev), 2);
}

static void test_ring_overflow(void)
{
    int i, more = 0;
    /* Snow sends up to 8 whatever alloc says: extra frames are skipped and counted */
    setup(FD_SNOW, 2, 0);
    for (i = 0; i < 8; i++)
        net_rx(60, (uint8_t)i);
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), 2);
    T_EQ(rx.st.overflow, 6);
    T_EQ(dev.abandoned, 0);
    T_EQ(dev.xfer_noinc, 6 * 68);   /* skipped bytes went to the sink */
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), 0);
    T_EQ(dn, 2);

    /* new fw: greedy alloc with small frames overflows a 2-slot ring ... */
    setup(FD_FW_2608, 2, 0);
    for (i = 0; i < 8; i++)
        net_rx(60, (uint8_t)i);
    T_EQ(poll_once(MP_READ_MULTI, 8, 0, &more), 2);
    T_EQ(rx.st.overflow, 6);
    /* ... the safe alloc never sends more packets than free slots */
    setup(FD_FW_2608, 2, 0);
    for (i = 0; i < 8; i++)
        net_rx(60, (uint8_t)i);
    T_EQ(drain(MP_READ_MULTI, 8, 1), 8);
    T_EQ(rx.st.overflow, 0);
    T_EQ(dn, 8);
}

static void full_cb(void *ctx)
{
    (void)ctx;
    deliver();
}

static void test_ring_full_drain(void)
{
    int i, more = 0;
    /* Snow sends 8 into a 2-slot ring: with on_full every frame is delivered, in order */
    setup(FD_SNOW, 2, 0);
    mp_rx_set_full(&rx, full_cb, NULL);
    for (i = 0; i < 8; i++)
        net_rx(60, (uint8_t)(i * 3));
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), 8);
    T_EQ(rx.st.overflow, 0);
    T_EQ(rx.st.drained, 3);         /* frames 3, 5 and 7 each found the ring full */
    T_EQ(dn, 8);
    for (i = 0; i < 8; i++)
        T_EQ(dfirst[i], (uint8_t)(i * 3));
    T_EQ(dev.xfer_noinc, 0);

    /* a handler that sends from inside the mid-READ delivery finds the bus busy: the reply is
     * deferred to the TX ring and goes out after the READ */
    setup(FD_SNOW, 2, 0);
    mp_rx_set_full(&rx, full_cb, NULL);
    echo = 1;
    for (i = 0; i < 4; i++)
        net_rx(60, (uint8_t)i);
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), 4);
    T_EQ(dn, 4);
    T_EQ(tx.st.queued, 2);          /* the two delivered mid-READ */
    T_EQ(tx.st.sent, 2);            /* the two delivered after it */
    while (mp_ring_used(&txq))
        T_CHECK(mp_tx_flush(&tx, &bus) > 0);
    T_EQ(dev.nsent, 4);

    /* a callback that frees nothing still ends in skip + overflow */
    setup(FD_SNOW, 1, 0);
    mp_rx_set_full(&rx, NULL, NULL);
    for (i = 0; i < 3; i++)
        net_rx(60, (uint8_t)i);
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), 1);
    T_EQ(rx.st.overflow, 2);
}

static void test_device_queue_overflow(void)
{
    int i;
    /* firmware ring of 20: the 20th unread enqueue makes it read as empty
     * (write index catches read index, network.c scsiNetworkEnqueue) */
    setup(FD_FW_2608, 8, 0);
    for (i = 0; i < 19; i++)
        net_rx(60, (uint8_t)i);
    T_EQ(fd_queued(&dev), 19);
    net_rx(60, 19);
    T_EQ(fd_queued(&dev), 0);
    T_EQ(drain(MP_READ_MULTI, 8, 1), 0);
    net_rx(60, 20);
    T_EQ(drain(MP_READ_MULTI, 8, 1), 1);
    T_EQ(dfirst[0], 20);
    /* Snow just queues */
    setup(FD_SNOW, 8, 0);
    for (i = 0; i < 20; i++)
        net_rx(60, (uint8_t)i);
    T_EQ(drain(MP_READ_MULTI, 8, 1), 20);
    /* enable clears the queue [fw, snow] */
    setup(FD_FW_2608, 8, 0);
    net_rx(60, 1);
    {
        uint8_t cdb[6];
        mp_cdb_enable(cdb, 1);
        cmd_none(cdb);
    }
    T_EQ(fd_queued(&dev), 0);
}

static void test_transfer_split_vs_pauses(void)
{
    int i;
    /* v2026.08.17 pauses 75 us after each header and 300 us between packets:
     * the header is read polled, so blind transfers never span a pause */
    setup(FD_FW_2608, 8, 0);
    for (i = 0; i < 4; i++)
        net_rx(300, (uint8_t)i);
    T_EQ(drain(MP_READ_MULTI, 0, 1), 4);
    T_EQ(dev.blind_gap_cross, 0);
    T_CHECK(dev.blind_gap_start > 0);   /* lead 0: blind starts right after the 75 us */
    setup(FD_FW_2608, 8, 0);
    for (i = 0; i < 4; i++)
        net_rx(300, (uint8_t)i);
    T_EQ(drain(MP_READ_MULTI, 8, 1), 4);
    T_EQ(dev.blind_gap_cross, 0);
    T_EQ(dev.blind_gap_start, 0);       /* lead 8: the pause falls in polled bytes */
    T_CHECK(dev.xfer_blind > 0);
    /* old fw too (80 us after the header) */
    setup(FD_FW_OLD, 8, 0);
    net_rx(300, 0);
    T_EQ(drain(MP_READ_MULTI, 8, 1), 1);
    T_EQ(dev.blind_gap_cross + dev.blind_gap_start, 0);
}

static void test_tx_single_all(void)
{
    static uint8_t a[14], b[1500];
    int k;
    for (k = 0; k < 3; k++) {
        mp_seg s[2];
        setup(kinds[k], 4, 0);
        make_frame(a, 14, 0x0800, 0);
        memset(b, 0x5C, sizeof(b));
        s[0].p = a; s[0].len = 14;
        s[1].p = b; s[1].len = 28;     /* runt: 42 bytes */
        T_EQ(mp_tx_send(&tx, &bus, s, 2), MP_OK);
        s[1].len = 1500;               /* max: 1514 */
        T_EQ(mp_tx_send(&tx, &bus, s, 2), MP_OK);
        T_EQ(dev.nsent, 2);
        T_EQ(dev.sent_len[0], 60);     /* padded to 60 */
        T_EQ(dev.sent[0][41], 0x5C);
        T_EQ(dev.sent[0][42], 0);
        T_EQ(dev.sent[0][59], 0);
        T_MEM(dev.sent[0] + 6, dev.mac, 6);   /* source MAC filled in */
        T_EQ(dev.sent_len[1], 1514);
        T_EQ(dev.sent[1][1513], 0x5C);
        T_EQ(dev.phase, FD_FREE);
        T_EQ(tx.st.sent, 2);
        T_EQ(tx.st.errors, 0);
    }
}

static void test_tx_batch(void)
{
    static uint8_t f[200];
    int k, i;
    for (k = 0; k < 3; k++) {
        mp_seg s;
        int r;
        setup(kinds[k], 4, 1);
        for (i = 0; i < 3; i++) {
            make_frame(f, (uint16_t)(100 + i), 0x0800, (uint8_t)(i * 3));
            s.p = f;
            s.len = (uint16_t)(100 + i);
            T_EQ(mp_tx_queue(&tx, &s, 1), MP_OK);
        }
        r = mp_tx_flush(&tx, &bus);
        if (kinds[k] == FD_FW_OLD) {
            /* old fw reads len + 8 and stops: frame 1 out, frame 2 fails,
             * batching is turned off and the rest go one per WRITE */
            T_EQ(r, MP_E_BUS);
            T_EQ(tx.batch, 0);
            T_EQ(tx.st.batch_off, 1);
            T_EQ(dev.nsent, 1);
            T_EQ(mp_ring_used(&txq), 2);
            T_EQ(mp_tx_flush(&tx, &bus), 1);
            T_EQ(mp_tx_flush(&tx, &bus), 1);
        } else {
            T_EQ(r, 3);
            T_EQ(tx.st.batches, 1);
            T_EQ(dev.writes, 1);
        }
        T_EQ(mp_tx_flush(&tx, &bus), 0);
        T_EQ(dev.nsent, 3);            /* each frame exactly once, in order */
        for (i = 0; i < 3; i++) {
            T_EQ(dev.sent_len[i], 100 + i);
            T_EQ(dev.sent[i][14], i * 3);
        }
        T_EQ(dev.phase, FD_FREE);
    }
}

static void test_busy_and_defer(void)
{
    static uint8_t f[60];
    mp_seg s;
    int more;
    setup(FD_FW_2608, 4, 1);
    net_rx(60, 1);
    dev.busy_next = 1;
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), MP_E_BUSY);
    T_EQ(rx.st.polls, 0);
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), 1);
    make_frame(f, 60, 0x0800, 9);
    s.p = f;
    s.len = 60;
    dev.busy_next = 1;
    T_EQ(mp_tx_send(&tx, &bus, &s, 1), MP_E_BUSY);   /* glue then queues it */
    T_EQ(mp_tx_queue(&tx, &s, 1), MP_OK);
    T_EQ(mp_tx_flush(&tx, &bus), 1);
    T_EQ(dev.nsent, 1);
}

static void test_bus_error_mid_payload(void)
{
    int more;
    setup(FD_SNOW, 4, 0);
    net_rx(500, 1);
    net_rx(500, 2);
    dev.fail_in_after = 6 + 504 + 6 + 100;   /* inside the second payload */
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), MP_E_BUS);
    T_EQ(rx.st.aborted, 1);
    T_EQ(dn, 1);                 /* the first frame was complete and stays */
    T_EQ(dev.phase, FD_FREE);
    net_rx(80, 3);
    T_EQ(poll_once(MP_READ_MULTI, 8, 1, &more), 1);
    T_EQ(dfirst[1], 3);
}

static void test_snow_check_condition(void)
{
    int more;
    /* Snow refuses a READ whose alloc can't hold one packet (the packet is lost) */
    setup(FD_SNOW, 4, 0);
    net_rx(1514, 1);
    T_EQ(mp_rx_poll(&rx, &bus, MP_READ_MULTI, 100, 0, &more), MP_E_BUS);
    T_EQ(dev.checks, 1);
    T_EQ(dev.phase, FD_FREE);
}

static void test_nested_echo(void)
{
    int k, i;
    for (k = 0; k < 3; k++) {
        setup(kinds[k], 8, 0);   /* Snow sends all 5 in one READ: 8 slots */
        echo = 1;
        for (i = 0; i < 5; i++)
            net_rx(80, (uint8_t)i);
        T_EQ(drain(MP_READ_MULTI, 8, 1), 5);
        T_EQ(dev.nsent, 5);
        T_EQ(mp_ring_used(&txq), 0);
        for (i = 0; i < 5; i++)
            T_EQ(dev.sent[i][14], i);
    }
}

static void test_inquiry_stats_wifi(void)
{
    uint8_t cdb[6], buf[200];
    int k;
    for (k = 0; k < 3; k++) {
        setup(kinds[k], 4, 0);
        mp_cdb_inquiry(cdb, 36);
        T_EQ(cmd_in(cdb, buf, 36), 0);
        T_CHECK(mp_dev_inquiry_match(buf, 36));
        T_MEM(tx.mac, dev.mac, 6);
        mp_cdb_mcast(cdb);
        T_EQ(bus.begin(bus.ctx, cdb), 0);
        T_EQ(bus.xout(bus.ctx, (const uint8_t *)"\x01\x00\x5e\x00\x00\x01", 6, MP_XF_POLLED), 0);
        T_EQ(bus.end(bus.ctx), 0);
        T_EQ(dev.nmcast, 1);
        mp_cdb_wifi(cdb, MP_WIFI_INFO, 2 + MP_WIFI_ENTRY_LEN);
        if (kinds[k] == FD_SNOW) {
            T_EQ(cmd_in(cdb, buf, 2 + MP_WIFI_ENTRY_LEN), MP_E_STATUS);   /* not emulated */
        } else {
            uint8_t join[MP_WIFI_JOIN_LEN];
            const uint8_t *first;
            mp_wifi_entry e;
            mp_cdb_wifi(cdb, MP_WIFI_JOIN, MP_WIFI_JOIN_LEN);
            mp_dev_wifi_join(join, "MoofNet", "pw", 6);
            T_EQ(bus.begin(bus.ctx, cdb), 0);
            T_EQ(bus.xout(bus.ctx, join, MP_WIFI_JOIN_LEN, MP_XF_POLLED), 0);
            T_EQ(bus.end(bus.ctx), 0);
            T_EQ(dev.wifi_joined, 1);
            mp_cdb_wifi(cdb, MP_WIFI_INFO, 2 + MP_WIFI_ENTRY_LEN);
            T_EQ(cmd_in(cdb, buf, 2 + MP_WIFI_ENTRY_LEN), 0);
            T_EQ(mp_dev_wifi_list(buf, 2 + MP_WIFI_ENTRY_LEN, &first), 1);
            mp_dev_wifi_entry(first, &e);
            T_CHECK(strcmp(e.ssid, "MoofNet") == 0);
            T_EQ(e.rssi, -50);
            mp_cdb_wifi(cdb, MP_WIFI_RESULTS, 200);
            T_EQ(cmd_in(cdb, buf, 2 + 2 * MP_WIFI_ENTRY_LEN), 0);
            T_EQ(mp_dev_wifi_list(buf, 2 + 2 * MP_WIFI_ENTRY_LEN, &first), 2);
        }
    }
}

static void test_interleaved_traffic(void)
{
    /* RX and TX mixed over many rounds, all personalities, batching on */
    static uint8_t f[600];
    int k, round;
    for (k = 0; k < 3; k++) {
        uint32_t want_rx = 0, want_tx = 0;
        setup(kinds[k], 4, 1);
        for (round = 0; round < 40; round++) {
            int n = round % 4, i;
            mp_seg s;
            for (i = 0; i < n; i++, want_rx++)
                net_rx((uint16_t)(42 + (round * 37 + i * 101) % 1473), (uint8_t)want_rx);
            for (i = 0; i < (round % 3); i++) {
                uint16_t len = (uint16_t)(20 + (round * 13 + i) % 580);
                make_frame(f, len, 0x0806, (uint8_t)want_tx);
                s.p = f;
                s.len = len;
                if (mp_tx_queue(&tx, &s, 1) == MP_OK)
                    want_tx++;
            }
            while (mp_tx_flush(&tx, &bus) > 0)
                ;
            drain(MP_READ_MULTI, 8, 1);
        }
        while (mp_tx_flush(&tx, &bus) > 0)
            ;
        T_EQ(dn, (int)want_rx);
        T_EQ(dev.nsent, want_tx);
        T_EQ(rx.st.overflow + rx.st.bad + rx.st.oversize + rx.st.aborted, 0);
        T_EQ(dev.abandoned, 0);
        T_EQ(dev.blind_gap_cross, 0);
    }
}

T_MAIN(test_empty_polls, test_rx_sizes, test_max_frame_content, test_multi_packet,
       test_single_mode_gate, test_fw_read_cap, test_ring_overflow, test_ring_full_drain,
       test_device_queue_overflow, test_transfer_split_vs_pauses, test_tx_single_all,
       test_tx_batch, test_busy_and_defer, test_bus_error_mid_payload,
       test_snow_check_condition, test_nested_echo, test_inquiry_stats_wifi,
       test_interleaved_traffic)

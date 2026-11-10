| enetbench_ph.s - self-contained Ethernet protocol-handler stub for EnetBench.
|
| EnetBench drives the .ENET0 (DaynaPORT) driver directly, with MacTCP closed, so it owns
| the protocol handlers. This stub is attached (EAttachPH) for both EtherTypes 0x0800 (IP)
| and 0x0806 (ARP). The driver enters it from a level-1 interrupt with the frame already in
| its buffer (docs/dev/MACTCP-INTERFACE.md "Ethernet driver calls", R1/R4 + obs):
|   A3 = just past the 14-byte data-link header, A4 = ReadPacket (ReadRest = A4+2),
|   D1 = bytes of the packet still unread, A0/A1 driver-private until ReadRest, SR = 0x2600.
| Return with RTS. The stub reads the frame with ONE ReadRest into the next free ring slot,
| recording for each frame: TickCount, SR at entry, the stack pointer, the frame length and
| the EtherType. The ring (EbRing) and slot (EbSlot) layouts are set in enetbench.c and
| checked there with _Static_assert; keep the .equ values in step.
|
| Based on MoofTCP's mac/glue/mt_enet_ph.s (same register convention), simplified to record
| straight into EnetBench's own ring instead of poking a gate.

        .equ    RING_HEAD,   0
        .equ    RING_TAIL,   2
        .equ    RING_DROPS,  4
        .equ    RING_NSLOTS, 6
        .equ    RING_SLOT0,  8
        .equ    NSLOTS,      8
        .equ    SLOT_SIZE,   1536
        .equ    SLOT_LEN,    0          | uint16 frame length (header included)
        .equ    SLOT_ETYPE,  2          | uint16 EtherType
        .equ    SLOT_SR,     4          | uint16 SR at entry
        .equ    SLOT_TICK,   8          | uint32 TickCount
        .equ    SLOT_SP,     12         | uint32 stack pointer at entry
        .equ    SLOT_BUF,    16         | frame bytes
        .equ    READ_MAX,    1520-14    | ReadRest D3: room for data after the header

        .text
        .align  2
        .globl  eb_ph
        .globl  eb_ph_set_ring

eb_ph_ring:
        .long   0

| void eb_ph_set_ring(EbRing *r): patch the ring pointer into the code (0 = detached)
eb_ph_set_ring:
        move.l  4(%sp),%d0
        lea     eb_ph_ring(%pc),%a0
        move.l  %d0,(%a0)
        rts

eb_ph:
        movem.l %d0-%d7/%a0-%a6,-(%sp)
        move.l  eb_ph_ring(%pc),%d0
        beq     .Ldrain                  | detached: still read the packet out
        move.l  %d0,%a2                  | a2 = EbRing
        move.w  RING_HEAD(%a2),%d2       | d2 = head (ReadRest preserves D2 and A0-A2)
        move.w  %d2,%d4
        addq.w  #1,%d4
        cmp.w   #NSLOTS,%d4
        bne     .Lnw
        moveq   #0,%d4
.Lnw:   cmp.w   RING_TAIL(%a2),%d4
        beq     .Lfull                   | ring full: drop
        mulu.w  #SLOT_SIZE,%d2
        lea     RING_SLOT0(%a2,%d2.l),%a5   | a5 = slot
        | record metadata
        move.w  %sr,SLOT_SR(%a5)
        move.l  0x16A,SLOT_TICK(%a5)     | TickCount low-memory global
        move.l  %sp,SLOT_SP(%a5)
        move.w  -2(%a3),%d5              | EtherType: last 2 bytes of the 14-byte header
        move.w  %d5,SLOT_ETYPE(%a5)
        | copy the 14-byte header into the slot buffer
        lea     -14(%a3),%a6             | source: the 14 header bytes before A3
        lea     SLOT_BUF(%a5),%a3        | destination (A3 now = buffer for ReadRest)
        moveq   #6,%d0
.Lh:    move.w  (%a6)+,(%a3)+            | 7 words
        dbf     %d0,.Lh
        move.w  #READ_MAX,%d3
        jsr     2(%a4)                   | ReadRest: A3 = one past the last byte read
        move.l  %a3,%d0
        lea     SLOT_BUF(%a5),%a0
        sub.l   %a0,%d0
        move.w  %d0,SLOT_LEN(%a5)        | stored length (header included)
        move.w  RING_HEAD(%a2),%d2       | commit: head = head + 1 (mod slots)
        addq.w  #1,%d2
        cmp.w   #NSLOTS,%d2
        bne     .Lc
        moveq   #0,%d2
.Lc:    move.w  %d2,RING_HEAD(%a2)
        bra     .Ldone
.Lfull: addq.w  #1,RING_DROPS(%a2)
.Ldrain:
        moveq   #0,%d3
        jsr     2(%a4)                   | ReadRest with nothing wanted: discards the packet
.Ldone: movem.l (%sp)+,%d0-%d7/%a0-%a6
        rts

| mp_drv_ph.s - the poll task's Time Manager entry, and the protocol-handler call with
| ReadPacket / ReadRest.
|
| Handler convention (docs/dev/INTERFACE.md; MoofTCP MACTCP-INTERFACE.md "Ethernet driver
| calls"; Inside Macintosh: Networking, "Writing a multinode receive routine"):
|   A3 = just past the 14-byte header, A4 = ReadPacket, A4+2 = ReadRest, D1 = bytes of the frame
|   still unread, counting the 4 CRC bytes (DaynaPORT's contract, which MacTCP was tuned on),
|   A0/A1 driver-private until ReadRest. The handler returns with RTS and may change D0-D3 and
|   A0-A3 only.
|   ReadPacket: D3 = bytes wanted, A3 = buffer. Copies them, D1 -= D3, A3 past the last byte,
|     D3 = 0, D0 = 0 with Z set. Asking for more than D1 gives D0 = -1 (Z clear), nothing read.
|   ReadRest: D3 = buffer size, A3 = buffer. Copies min(D3, D1) bytes, A3 past the last byte,
|     D3 = size - left (0 exact, > 0 room left over, < 0 bytes cut off), D1 = 0, D0 = 0, Z set.
|   Both keep D2 and A1-A2. A0 is our read cursor into the RX slot.
| The frame is already complete in an RX slot; the call happens after the SCSI transaction
| ended, at the poll task's interrupt level, on whatever stack was current.
        .text
        .align  2
        .globl  mp_tm_entry
        .globl  mp_wd_entry
        .extern mp_drv_wd
        .globl  mp_call_ph
        .extern mp_drv_tick

| Time Manager task: A1 = the TMTask (the first member of MpTimer); D0-D2/A0-A1 are free.
mp_tm_entry:
        move.l  %a1,-(%sp)
        jsr     mp_drv_tick(%pc)
        addq.l  #4,%sp
        rts

| The watchdog VBL task: A0 = the VBLTask. A VBL task may use D0-D3/A0-A3; C keeps D2-D3/A2-A3.
mp_wd_entry:
        move.l  %a0,-(%sp)
        jsr     mp_drv_wd(%pc)
        addq.l  #4,%sp
        rts

| void mp_call_ph(void *handler, uint8_t *frame, unsigned long len): len counts the CRC bytes.
mp_call_ph:
        movem.l %d2-%d7/%a2-%a6,-(%sp)   | 11 registers, 44 bytes
        move.l  48(%sp),%a2              | handler
        move.l  52(%sp),%a3              | frame
        move.l  56(%sp),%d1              | len
        lea     14(%a3),%a3              | past the header
        sub.l   #14,%d1                  | bytes left
        move.l  %a3,%a0                  | read cursor
        lea     mp_readpacket(%pc),%a4
        jsr     (%a2)
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

mp_readpacket:
        bra.s   .Lrp                     | A4: ReadPacket
                                         | A4+2: ReadRest
        move.w  %d3,%d0
        sub.w   %d1,%d0                  | result: size - left
        cmp.w   %d1,%d3
        bls.s   .Lrr1                    | size <= left: copy size
        move.w  %d1,%d3                  | else copy what is left
.Lrr1:  move.w  %d0,%d1                  | keep the result (D1 ends 0 anyway)
        bsr.s   mp_copy
        move.w  %d1,%d3
        ext.l   %d3
        moveq   #0,%d1
        moveq   #0,%d0                   | Z set
        rts

.Lrp:   cmp.w   %d1,%d3
        bhi.s   .Lrp9                    | more than is left
        sub.w   %d3,%d1
        bsr.s   mp_copy
        moveq   #0,%d3
        moveq   #0,%d0                   | Z set
        rts
.Lrp9:  moveq   #-1,%d0                  | Z clear
        rts

| Copy D3.w bytes from (A0)+ to (A3)+; changes D0 and D3 only. The 68000 faults on a word or long
| access at an odd address, so: when source and destination differ in parity, bytes; else one
| byte to make both even, then 16 bytes per pass with move.l, then the tail by its bits. About
| 6 cycles a byte against 22 for move.b/dbf (a full frame: about 1 ms instead of 4).
mp_copy:
        tst.w   %d3
        beq.s   .Lc9
        move.l  %a0,%d0
        sub.l   %a3,%d0
        btst    #0,%d0
        bne.s   .Lcb                     | parities differ: bytes
        move.w  %a0,%d0
        btst    #0,%d0
        beq.s   .Lc1
        move.b  (%a0)+,(%a3)+            | both odd: one byte, then both even
        subq.w  #1,%d3
.Lc1:   move.w  %d3,%d0
        lsr.w   #4,%d0                   | 16-byte passes
        bra.s   .Lc3
.Lc2:   move.l  (%a0)+,(%a3)+
        move.l  (%a0)+,(%a3)+
        move.l  (%a0)+,(%a3)+
        move.l  (%a0)+,(%a3)+
.Lc3:   dbf     %d0,.Lc2
        btst    #3,%d3
        beq.s   .Lc4
        move.l  (%a0)+,(%a3)+
        move.l  (%a0)+,(%a3)+
.Lc4:   btst    #2,%d3
        beq.s   .Lc5
        move.l  (%a0)+,(%a3)+
.Lc5:   btst    #1,%d3
        beq.s   .Lc6
        move.w  (%a0)+,(%a3)+
.Lc6:   btst    #0,%d3
        beq.s   .Lc9
        move.b  (%a0)+,(%a3)+
.Lc9:   rts
.Lcb:   subq.w  #1,%d3
.Lcb1:  move.b  (%a0)+,(%a3)+
        dbf     %d3,.Lcb1
        rts

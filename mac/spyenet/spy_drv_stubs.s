| spy_drv_stubs.s - SpyENET spy driver entry dispatcher, forward-completion routine and the
| wrapped Ethernet protocol-handler stub. See spy_drv.c and README.md.
|
| The spy DRVR resource (spyenet.r) is: the 18-byte header, the name "\p.ENET0" padded to
| offset 24, six two-instruction stubs (open/prime/control/status/close/boot) that load a
| selector into D0 and branch to offset 48 where this flat Retro68 block starts. Offset 0 is
| Retro68's _entry_trampoline, which jumps to `_start` with all registers intact. So `_start`
| sees D0 = selector and, for 0..4, the Device Manager's A0 = parameter block, A1 = DCE.
| (Same recipe as MoofTCP's mac/glue/mt_drv_stubs.s + mac/moof/moof.r.)
|
| Dispatch to C: spy_open/spy_prime/spy_control/spy_status/spy_close(pb, dce). Each returns a
| result in D0. Prime, control and status are queued calls: they finish through jIODone unless
| immediate. A control the spy forwarded async returns 1 (= "I will complete it later via the
| forward completion"); we then just RTS without touching jIODone. Open and close return with
| RTS. Every register but D0 is preserved for the Device Manager.
        .text
        .align  2
        .globl  _start
        .globl  spy_fwd_complete
        .globl  spy_ph
        .globl  spy_scsi_patch
        .extern spy_open
        .extern spy_prime
        .extern spy_control
        .extern spy_status
        .extern spy_close
        .extern spy_log_handler
        .extern spy_find_handler

        .equ    JIODONE, 0x08FC

| D0 = selector (0 open,1 prime,2 control,3 status,4 close,5 boot); A0 = PB, A1 = DCE.
_start:
        cmp.w   #5,%d0
        beq     .Lboot
        movem.l %d1-%d7/%a0-%a6,-(%sp)
        move.l  %a1,-(%sp)               | dce
        move.l  %a0,-(%sp)               | pb
        cmp.w   #2,%d0
        beq     .Lctl
        cmp.w   #1,%d0
        beq     .Lprime
        cmp.w   #3,%d0
        beq     .Lstat
        tst.w   %d0
        beq     .Lopen
        jsr     spy_close
        bra     .Lret_imm
.Lopen: jsr     spy_open
        bra     .Lret_imm
.Lstat: jsr     spy_status
        bra     .Lret_q
.Lctl:  jsr     spy_control
        bra     .Lret_q
.Lprime:
        jsr     spy_prime
        bra     .Lret_q

| open/close return: the Device Manager stores D0 in ioResult.
.Lret_imm:
        addq.l  #8,%sp
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        ext.l   %d0
        rts

| queued return (control/prime/status): D0 from C is 1 when we forwarded async and will IODone the
| PB ourselves later (spy_fwd_complete) -> just RTS; otherwise finish via jIODone unless the
| call was immediate (ioTrap bit 9).
.Lret_q:
        addq.l  #8,%sp
        cmp.w   #1,%d0                    | "complete later"?
        beq     .Llater
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        ext.l   %d0
        btst    #1,6(%a0)                 | ioTrap bit 9 (immediate)
        bne     .Limm
        move.l  JIODONE,-(%sp)            | jIODone: A1 = DCE, A0 = PB, D0 = result
.Limm:  rts
.Llater:
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        moveq   #0,%d0                    | tell the Device Manager "accepted, async"
        ext.l   %d0
        rts

| boot (selector 5): the INIT calls this once, after installing both units, to relocate this
| flat block in place (Retro68's relocations use absolute addresses that are only valid after
| this runs). Same relocation sequence as MoofTCP's mt_drv_stubs.s .Lboot. A C call (spy_boot);
| no arguments.
.Lboot:
        lea     .Lb1(%pc),%a0
.Lb1:   move.l  #.Lb1,%d1                | link address (patched by the relocation)
        sub.l   %d1,%a0
        move.l  %a0,%d1
        and.l   #0x00FFFFFF,%d1          | 24-bit clean displacement (as StripAddress24)
        move.l  %d1,%a0
        add.l   #Retro68Relocate,%a0     | its run-time address before relocation
        jsr     (%a0)                    | relocate the block in place (once)
        jmp     spy_boot                 | absolute: patched by the relocation just done

        .extern Retro68Relocate
        .extern spy_boot

| spy_fwd_complete - ioCompletion of a forwarded async PB (the copy of an async EWrite). A0 =
| the copy PB (the first member of a SpyFwd slot, spy_drv.c). We call C to copy the result
| back into the original PB and free the slot; C returns the original PB pointer in D0. We then
| jIODone the ORIGINAL PB against our own DCE (spy_our_dce, a resident C global), which
| dequeues it from our unit's queue and fires MacTCP's ioCompletion / releases its sync wait.
| The copy PB was queued on the REAL unit and has already been dequeued by the real driver's
| IODone before entering here, so nothing of ours leaks. Completion convention: A0 = PB.
spy_fwd_complete:
        move.l  %a0,-(%sp)               | arg: copy PB
        jsr     spy_fwd_done_c           | C: copy result into orig, free slot; D0 = original PB
        addq.l  #4,%sp
        move.l  %d0,%a0                  | A0 = original PB
        move.w  16(%a0),%d0              | D0 = result (C already stored it in orig ioResult)
        ext.l   %d0
        move.l  spy_our_dce,%a1          | A1 = our DCE (resident C global; abs after relocation)
        move.l  JIODONE,-(%sp)           | jIODone: A0 = PB, A1 = DCE, D0 = result
        rts

        .extern spy_fwd_done_c
        .extern spy_our_dce

| spy_ph - the wrapped protocol handler. The real driver calls this (we attached it in place of
| MacTCP's handler). Entry (MoofTCP docs/dev/MACTCP-INTERFACE.md): A3 = past the 14-byte header,
| A4 = ReadPacket (ReadRest = A4+2), D1 = bytes left, SR = 0x2600. Only D0-D3/A0-A3 may change, so
| the chain to MacTCP's handler goes through the stack: every register is restored and RTS enters
| the handler, whose own RTS comes back to .Lph_back. spy_ph_depth tells spy_control an EWrite
| came from inside a handler. If no handler is found we drain the packet.
spy_ph:
        pea     .Lph_back(%pc)           | MacTCP's handler returns here
        clr.l   -(%sp)                   | slot for its address
        movem.l %d0-%d7/%a0-%a6,-(%sp)
        addq.w  #1,spy_ph_depth
        move.w  -2(%a3),%d3              | EtherType (the last 2 bytes of the header)
        move.w  %sr,%d4                  | SR at entry
        move.w  %d1,%d5                  | bytes left (frame length - 14 + 4)
        | log: spy_log_handler(uint16 etherType, uint16 sr, uint16 d1len). This ABI gives a
        | uint16_t argument 2 bytes of stack, not 4.
        move.w  %d5,-(%sp)
        move.w  %d4,-(%sp)
        move.w  %d3,-(%sp)
        jsr     spy_log_handler
        addq.l  #6,%sp
        | find MacTCP's handler for this EtherType: spy_find_handler(uint16) -> ptr in D0
        move.w  %d3,-(%sp)
        jsr     spy_find_handler
        addq.l  #2,%sp
        move.l  %d0,60(%sp)              | the slot, just above the 15 saved registers
        beq     .Lnohand
        movem.l (%sp)+,%d0-%d7/%a0-%a6
        rts                              | into MacTCP's handler, registers intact
.Lph_back:
        subq.w  #1,spy_ph_depth
        rts
.Lnohand:
        movem.l (%sp)+,%d0-%d7/%a0-%a6
        addq.l  #8,%sp                   | drop the slot and .Lph_back
        subq.w  #1,spy_ph_depth
        moveq   #0,%d3
        jsr     2(%a4)                   | ReadRest nothing: discard the packet
        rts

        .extern spy_ph_depth

| spy_scsi_patch - head patch on _SCSIDispatch (A815): log the call (spy_log_scsi, spy_drv.c),
| then continue in the real trap with every register and the stack as they were. At entry
| (SP) = the return address, 4(SP) = the selector, then the call's arguments.
spy_scsi_patch:
        movem.l %d0-%d2/%a0-%a1,-(%sp)   | 20 bytes
        move.w  %sr,%d0
        move.w  %d0,-(%sp)               | arg 2: SR (uint16_t: 2 bytes of stack)
        pea     22(%sp)                  | arg 1: the caller's stack (return address at 0)
        jsr     spy_log_scsi
        addq.l  #6,%sp
        movem.l (%sp)+,%d0-%d2/%a0-%a1
        move.l  spy_scsi_orig,-(%sp)
        rts

        .extern spy_log_scsi
        .extern spy_scsi_orig

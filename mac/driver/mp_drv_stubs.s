| mp_drv_stubs.s - MoofPort driver entry dispatcher (same recipe as SpyENET and MoofTCP).
|
| The DRVR resource (moofport.r) is the 18-byte header, the name "\p.ENET0" padded to offset 26,
| six two-instruction stubs (open, prime, control, status, close, boot) that load a selector into
| D0 and branch to offset 50, where this flat Retro68 block starts. Offset 0 of the block is
| Retro68's entry trampoline, which reaches _start with every register intact. So _start sees
| D0 = selector and, for 0..4, the Device Manager's A0 = parameter block, A1 = DCE.
|
|   0 open, 4 close: C result in D0, RTS (the Device Manager stores it in ioResult).
|   1 prime, 2 control, 3 status: queued calls. They end through jIODone (A1 = DCE, D0 =
|     result), or with RTS when the call is immediate (ioTrap bit 9). Control may return 1
|     (PENDING in mp_drv.c): an async EWrite waiting for a TX slot. Then this returns with RTS
|     and D0 = 0 and leaves the call in progress; the poll task completes it with mp_iodone.
|   5 boot (from the INIT, C convention): jump to mp_drv_boot with the INIT's argument still on
|     the stack. The block is position-independent (-mpcrel, PC-relative calls here), so it needs
|     no relocation and Retro68's relocator is not linked in.
| Every register but D0 is preserved for the Device Manager.
        .text
        .align  2
        .globl  _start
        .globl  mp_iodone
        .extern mp_drv_open
        .extern mp_drv_prime
        .extern mp_drv_control
        .extern mp_drv_status
        .extern mp_drv_close
        .extern mp_drv_boot

        .equ    JIODONE, 0x08FC

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
        jsr     mp_drv_close(%pc)
        bra     .Lret
.Lopen: jsr     mp_drv_open(%pc)
.Lret:  addq.l  #8,%sp
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        ext.l   %d0
        rts
.Lctl:  jsr     mp_drv_control(%pc)
        bra     .Lfin
.Lprime:
        jsr     mp_drv_prime(%pc)
        bra     .Lfin
.Lstat: jsr     mp_drv_status(%pc)
.Lfin:  addq.l  #8,%sp
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        ext.l   %d0
        cmp.w   #1,%d0
        beq     .Lpend
        btst    #1,6(%a0)                | ioTrap bit 9 (high byte, bit 1): immediate
        bne     .Limm
        move.l  JIODONE,-(%sp)           | jIODone: A1 = DCE, D0 = result
.Limm:  rts
.Lpend: moveq   #0,%d0                   | in progress: IODone comes later
        rts

| void mp_iodone(void *dce, long result): complete the pending call (from the poll task, or
| Close). IODone may start the next queued call, so this can re-enter the driver.
mp_iodone:
        movem.l %d1-%d7/%a0-%a6,-(%sp)   | 14 registers, 56 bytes
        move.l  60(%sp),%a1              | dce
        move.l  64(%sp),%d0              | result
        move.l  JIODONE,%a0
        jsr     (%a0)
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        rts

.Lboot:
        jmp     mp_drv_boot(%pc)

| scsiprobe_tm.s - Time Manager task trampoline for ScsiProbe probe 6.
|
| The Time Manager enters a task proc with A1 = the TMTask record (68k convention). This
| trampoline saves the scratch registers, loads CurrentA5 (low memory 0x904) so the C code
| it calls can reach the application's A5 globals at interrupt time (the same trick
| enetspike.c / MoofTCP use in their interrupt handlers), calls the C routine sp_tm_tick(),
| then restores and returns with RTS. sp_tm_tick re-arms the task itself with PrimeTime.

        .text
        .align  2
        .globl  sp_tm_entry
        .extern sp_tm_tick

sp_tm_entry:
        movem.l %d0-%d2/%a0-%a1,-(%sp)
        move.l  %a5,-(%sp)
        move.l  0x904,%a5               | CurrentA5: the application's A5 world
        jsr     sp_tm_tick
        move.l  (%sp)+,%a5
        movem.l (%sp)+,%d0-%d2/%a0-%a1
        rts

| sp_clk_nop: the lap timer's task proc (probes 8 and 9). The lap timer is always removed long
| before it fires; if it ever did, it must do nothing.
        .globl  sp_clk_nop
sp_clk_nop:
        rts

| tcpsend_tm.s - TcpSend's stopwatch: the Time Manager entry of its clock task (tcpsend.c).
| A1 = the TMTask, the first member of the Clock struct. No A5 is needed: ts_clk_fire only
| touches that struct and calls PrimeTime.
        .text
        .align  2
        .globl  ts_clk_entry
        .extern ts_clk_fire

ts_clk_entry:
        movem.l %d0-%d2/%a0-%a1,-(%sp)
        move.l  %a1,-(%sp)
        jsr     ts_clk_fire
        addq.l  #4,%sp
        movem.l (%sp)+,%d0-%d2/%a0-%a1
        rts

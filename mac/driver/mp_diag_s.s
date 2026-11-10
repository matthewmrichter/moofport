| mp_diag_s.s - the hwsend diagnostic clock's Time Manager entry (mp_diag.c; -DMP_DIAG build only).
| The Time Manager calls it with A1 = the TMTask, the first member of MpDiag; D0-D2/A0-A1 are free.
        .text
        .align  2
        .globl  mp_diag_clk_entry
        .globl  mp_diag_vbl_entry
        .extern mp_diag_clk_fire
        .extern mp_diag_vbl_fire

mp_diag_clk_entry:
        move.l  %a1,-(%sp)
        jsr     mp_diag_clk_fire(%pc)
        addq.l  #4,%sp
        rts

| The VBL task (tick phase): A0 = the VBLTask, the first member of struct MpDiagVbl.
| A VBL task may use D0-D3/A0-A3; the C routine keeps D2-D3/A2-A3.
mp_diag_vbl_entry:
        move.l  %a0,-(%sp)
        jsr     mp_diag_vbl_fire(%pc)
        addq.l  #4,%sp
        rts

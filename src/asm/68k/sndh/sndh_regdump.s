    dc.w    vars_size
    bra.w   init
    bra.w   exit
    bra.w   play

init:
    movem.l d0/a0/a1,-(sp)
    subq.w  #1,d0
    lsl.l   #2,d0
    lea     data(pc),a0
    move.l  (a0,d0),d0
    add.l   a0,d0
    lea     curpos+2(pc),a0
    move.l  d0,(a0)
    bsr     exit
    ; setup irqs for PWM mode
    lea     irq_a1-(curpos+2)(a0),a1
    move.l  a1,(irq_a0+10)-(curpos+2)(a0)
    lea     irq_a0-(curpos+2)(a0),a1
    move.l  a1,(irq_a1+10)-(curpos+2)(a0)
    move.l  a1,$134
    lea     irq_b1-(curpos+2)(a0),a1
    move.l  a1,(irq_b0+10)-(curpos+2)(a0)
    lea     irq_b0-(curpos+2)(a0),a1
    move.l  a1,(irq_b1+10)-(curpos+2)(a0)
    move.l  a1,$120
    lea     irq_d1-(curpos+2)(a0),a1
    move.l  a1,(irq_d0+10)-(curpos+2)(a0)
    lea     irq_d0-(curpos+2)(a0),a1
    move.l  a1,(irq_d1+10)-(curpos+2)(a0)
    move.l  a1,$110
    ; enable irqs
    or.b    #%00100001,$fffffa07 ; timer A, B interrupt enable
    or.b    #%00100001,$fffffa13 ; unmask timer A, B interrupts
    bset    #4,$fffffa09 ; timer D interrupt enable
    bset    #4,$fffffa15 ; unmask timer D interrupts
    ; clear vars
    lea     vars(pc),a0
    moveq.l #vars_size/4,d0
.clearloop:
    clr.l   (a0)+
    dbne    d0,.clearloop
    movem.l (sp)+,d0/a0/a1
    rts

exit:
    ; stop timers
    clr.l   $fffffa18
    and.b   #~$f,$fffffa1d
    ; mute psg
    move.l  #$07003f00,$ffff8800
    move.l  #$08000000,$ffff8800
    move.l  #$09000000,$ffff8800
    move.l  #$0a000000,$ffff8800
    rts

play:
    movem.l d0-d3/a0-a5,-(sp)
    lea     vars(pc),a2
    subq.w  #1,waitctr(a2)
    bpl     play_end

curpos:
    move.l  #$88008800,a0
    move.l  #$ffff8800,a1
    move.l  #$fffffa19,a3
    move.w  backrefpos(a2),d1
cmdloop:
    moveq.l #0,d0
    move.b  (a0)+,d0
    bpl     cmd_backref
    cmp.b   #$ff,d0
    beq     cmd_endofstream
    cmp.b   #$d0,d0
    bcs     .one
    lsl.w   #8,d0
    move.b  (a0)+,d0
    move.w  d0,backref(a2,d1)
    addq.b  #2,d1
    bra     cmd_2
.one:
    cmp.b   #$b0,d0
    bcs     cmd_volume
    sub.b   #$b0,d0
cmd_setwait:
    move.w  d0,curwait(a2)
cmddone:
    move.w  curwait(a2),waitctr(a2)
    move.w  d1,backrefpos(a2)
    lea     curpos+2(pc),a1
    move.l  a0,(a1)
play_end:
    movem.l (sp)+,d0-d3/a0-a5
    rts

cmd_endofstream:
    ; mute upon end of stream
    ; placeholder until looping can be implemented
    bsr     exit
    bra     play_end

cmd_backref:
    lsl.w   #1,d0
    move.w  backref(a2,d0),d0
cmd_2:
    cmp.w   #$e000,d0
    bcc     cmd_notdirect
    and.w   #$0fff,d0
psgwrite:
    movep.w d0,0(a1)
    move.l  d0,d2
    lsr.w   #8,d2
    cmp.w   #13,d2
    bne     cmdloop
    move.b  d0,curenv(a2)
    btst    #3,lasttimer(a2)
    beq     .nosyncbuzz1
    move.b  d0,irq_a0+4-vars(a2)
.nosyncbuzz1:
    btst    #3,lasttimer+2(a2)
    beq     .nosyncbuzz2
    move.b  d0,irq_b0+4-vars(a2)
.nosyncbuzz2:
    btst    #3,lasttimer+4(a2)
    beq     cmdloop
    move.b  d0,irq_d0+4-vars(a2)
    bra     cmdloop

cmd_notdirect:
    cmp.w   #$e600,d0
    bcs     .notlongwait
    sub.w   #$e600-16,d0
    bra     cmd_setwait
.notlongwait:
    cmp.w   #$e300,d0
    bcc     cmd_timermode
cmd_timerperiod:
    move.l  d0,d2
    lsr.w   #8,d2
    sub.b   #$e0,d2
    cmp.b   #$2,d2
    bcs     .notch3
    addq.b  #1,d2
.notch3:
    lsl.w   #1,d2
    move.b  d0,6(a3,d2)
    bra     cmdloop

cmd_timermode:
    move.l  d0,d2
    lsr.w   #8,d2
    sub.b   #$e3,d2
    lea     irq_a0(pc),a4
    cmp.b   #1,d2
    bcs     .gotchan
    lea     irq_b0-irq_a0(a4),a4
    beq     .gotchan
    lea     irq_d0-irq_b0(a4),a4
.gotchan:
    lsl.w   #1,d2
    btst    #3,d0
    bne     .syncbuzz
    ; pwm
    btst    #3,lasttimer(a2,d2)
    beq     .nochange1
    bsr     .setvec
    ; switch to x1
    lea     irq_a1-irq_a0(a4),a5
    move.l  a5,10(a4)
    move.l  d0,d3
    lsr.w   #8,d3
    sub.b   #$e3-8,d3
    move.b  d3,2(a4)
.nochange1:
    ; store lower bound
    move.b  d0,d3
    lsr.b   #4,d3
    move.b  d3,4(a4)
    bra     .done

.syncbuzz:
    btst    #3,lasttimer(a2,d2)
    bne     .done
    bsr     .setvec
    ; only run x0
    move.l  a4,10(a4)
    move.b  #13,2(a4)
    move.b  curenv(a2),4(a4)
.done:
    move.b  d0,lasttimer(a2,d2)
    and.b   #7,d0
    and.b   #~15,(a3,d2)
    or.b    d0,(a3,d2)
    bra     cmdloop

.setvec:
    ; temporarily stop the timer
    and.b   #~$f,(a3,d2)
    ; get destination vector and set
    move.l  #0,a5
    move.w  14(a4),a5
    move.l  a4,(a5)
    rts

cmd_volume:
    lsl.w   #4,d0   ; 0000aaaa vvvv0000
    lsr.b   #4,d0   ; 0000aaaa 0000vvvv
    ; HACK: only write PWM upper bound from here
    ; since all non-env volume writes go here
    lea     irq_d1+4(pc),a4
    cmp.w   #$0a00,d0
    bcc     .done
    lea     irq_b1-irq_d1(a4),a4
    cmp.w   #$0900,d0
    bcc     .done
    lea     irq_a1-irq_b1(a4),a4
.done:
    move.b  d0,(a4)
    bra     psgwrite

irq_a0:
    move.l  #$08000000,$ffff8800
    move.l  #$88008800,$134
    bclr.b  #5,$fffffa0f
    rte
irq_a1:
    move.l  #$08000000,$ffff8800
    move.l  #$88008800,$134
    bclr.b  #5,$fffffa0f
    rte
irq_b0:
    move.l  #$09000000,$ffff8800
    move.l  #$88008800,$120
    bclr.b  #0,$fffffa0f
    rte
irq_b1:
    move.l  #$09000000,$ffff8800
    move.l  #$88008800,$120
    bclr.b  #0,$fffffa0f
    rte
irq_d0:
    move.l  #$0a000000,$ffff8800
    move.l  #$88008800,$110
    bclr.b  #4,$fffffa11
    rte
irq_d1:
    move.l  #$0a000000,$ffff8800
    move.l  #$88008800,$110
    bclr.b  #4,$fffffa11
    rte

    cnop    0,4
vars:
backrefpos: so.w 1
lasttimer:  so.w 3
waitctr:    so.w 1
curwait:    so.w 1
curenv:     so.w 2
backref:    so.b 256

    assert  (__SO&3)==0,"vars size are not in multiple of 4"
vars_size = __SO
data = vars+vars_size

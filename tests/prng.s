        .set noreorder
        .set noat
        .text
        lui   $t0, 0x8000            # 1
        lw    $a0, 0x80($t0)         # 2  unseeded: state(1) = step(ACE12468)
        lw    $a1, 0x80($t0)         # 3  state(2)
        lui   $t1, 0xDEAD            # 4
        ori   $t1, $t1, 0xBEEF       # 5
        sw    $t1, 0x80($t0)         # 6  store DEADBEEF (c)
        lw    $s0, 0x80($t0)         # 7  c+1: stale = state(6) = step^5(ACE12468)
        lw    $s1, 0x80($t0)         # 8  c+2: DEADBEEF
        lw    $s2, 0x80($t0)         # 9  c+3: step1(DEADBEEF)
        nop                          # 10
        lw    $s3, 0x80($t0)         # 11 c+5: step3(DEADBEEF)
        sw    $zero, 0x80($t0)       # 12 seed 0 -> 1
        nop                          # 13
        lw    $s4, 0x80($t0)         # 14 1
        addiu $t2, $zero, 0xAB       # 15
        sb    $t2, 0x83($t0)         # 16 seed 0x000000AB (lane +3)
        nop                          # 17
        lw    $s5, 0x80($t0)         # 18 000000AB
        sb    $t2, 0x80($t0)         # 19 seed 0xAB000000 (lane +0)
        nop                          # 20
        lw    $s6, 0x80($t0)         # 21 AB000000
        lbu   $s7, 0x83($t0)         # 22 low byte of step1(AB000000)
        sw    $t1, 0x180($t0)        # 23 0x80000180: no longer a PRNG alias (addra[11:6] decoded), dropped
        sw    $zero, 0x18($t0)       # 24 CNT_RESET: must not disturb PRNG
        lw    $t3, 0x80($t0)         # 25 state(24) = step4(AB000000) = 0AB00000
        lui   $t4, 0x0001            # 26 loop 65536 iterations x2 insns
1:      addiu $t4, $t4, -1           # 
        bne   $t4, $zero, 1b         #
        nop                          # (delay slot -> 3 insns/iter)
        lw    $t5, 0x80($t0)         # 26+3*65536+1 = 196635: state(196634) = step^196614(AB000000)
        break

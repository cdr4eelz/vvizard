# lcd.s - 2x16 LCD MMIO (0x80000100 + 4*i) and the narrowed MMIO decode (make test-hex "lcd").
# Built by the vvizard Makefile (make tests) into build/tests/lcd.bin; tests/lcd.hex is the fallback copy.
# vv-load: 0x40000000
# vv-isa: -mips1
        .set noreorder
        .set noat
        .text
        lui   $t0, 0x8000
        li    $t1, 'H'
        sw    $t1, 0x100($t0)        # LCD[0]  = 'H'   (sw: low byte)
        li    $t1, 0x12345669        # 'i' in the low byte, junk above
        sw    $t1, 0x104($t0)        # LCD[1]  = 'i'
        li    $t1, '!'
        sb    $t1, 0x10B($t0)        # LCD[2]  = '!'   (sb to byte +3 = dina[7:0])
        li    $t1, 'X'
        sb    $t1, 0x10C($t0)        # LCD[3]  = 0x00 with mmio-lane (sb +0 -> dina[31:24]); 'X' without
        sh    $t1, 0x112($t0)        # LCD[4]  = 'X'   (sh to +2 = dina[15:0])
        sh    $t1, 0x114($t0)        # LCD[5]  = 0x00 with mmio-lane; 'X' without
        li    $t1, 'L'
        sw    $t1, 0x13C($t0)        # LCD[15] = 'L'   (end of line 1)
        li    $t1, 'a'
        sw    $t1, 0x140($t0)        # LCD[16] = 'a'   (start of line 2)
        li    $t1, 'z'
        sw    $t1, 0x17C($t0)        # LCD[31] = 'z'   (last char)
        li    $t1, 'q'
        sw    $t1, 0x4100($t0)       # 16 KiB alias 0x80004100 -> LCD[0] = 'q'
        li    $t1, 'Q'
        sw    $t1, 0x4100($t0)       # ... and back to 'Q'
        li    $t1, 'U'
        sw    $t1, 0x108($t0)        # old UART TX alias: now LCD[2] = 'U', nothing on the UART
        sw    $t1, 0x180($t0)        # 0x80000180: neither registers nor LCD -> dropped
        sw    $t1, 0x208($t0)        # 0x80000208: old UART TX alias -> dropped
        li    $t1, '\n'
        sw    $t1, 0x4008($t0)       # 0x80004008: 16 KiB alias of UART TX -> sends '\n'
        lw    $t2, 0x100($t0)        # LCD read -> 0
        lw    $t3, 0x104($t0)        # LCD read -> 0
        lw    $t4, 0x0($t0)          # UART TX_READY -> 1
        lw    $t5, 0x100($t0)        # TX_READY alias at 0x80000100? no: LCD -> 0
        lw    $t6, 0x4000($t0)       # 16 KiB alias of TX_READY -> 1
        lw    $t7, 0x184($t0)        # unmapped offset reads 0 (HW: 32'dx)
        break

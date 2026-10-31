# uart.s - UART TX data lane, TX_READY, and the 0x80000108 LCD cell (make test-hex "uart").
# vv-load: 0x40000000
# vv-isa: -mips1
.set noreorder
.set noat
.text
.global _start
_start:
# UART TX: 'H' at 0x80000008; 0x80000108 is an LCD cell now (no TX alias); sb '\n' to the TX
# byte lane (0x8000000b, dina[7:0] under the mmio-lane quirk).  Each TX store waits for TX_READY
# (a store while busy is lost: tx-busy).  $t2 = TX_READY right after the last store (0: busy).
    lui $t0,0x8000
    addiu $t1,$zero,72
    sw $t1,8($t0)                            # 'H'
    addiu $t1,$zero,105
    sw $t1,264($t0)                          # 'i' -> LCD[2]
1:  lw $t3,0($t0)                            # wait until 'H' has gone
    nop
    beq $t3,$zero,1b
    nop
    addiu $t1,$zero,10
    sb $t1,11($t0)                           # '\n'
    lw $t2,0($t0)
    break

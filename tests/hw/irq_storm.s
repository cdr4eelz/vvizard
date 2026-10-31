# irq_storm.s - tests/hw/timer.inc with an ISR (tests/hw/isr.s, mode 1) that returns twice with the
# timer cause still pending and enabled: COP0150 re-enters at once (irq-storm guest bug).
# vv-load: 0x40000000
# vv-isa: -mips32r2
.equiv MODE, 1
.equiv ENTRIES, 3
.include "hw/hw.inc"
.include "hw/timer.inc"

/* memmap.h - single authoritative memory-map table for VVIZARD.
 * Sources (read-only): hardware/src/cpu/MemBank.v (top-nibble decode, BRAM addr[13:2]),
 * hardware/src/cpu/StageMW.v (fetch/read/write table), hardware/src/cpu/MemMapIO.v,
 * software/common/mmio_intr_cop0.h, software/proj_app6/shared6.h, software/common/graphics.h.
 *
 * Decode model: hardware only looks at ADDR[31:28] (the "nibble") to pick a device and
 * then only the low address bits that device has.  Everything inside a nibble ALIASES:
 *   BRAMs (BIOS/ISR/I-scratch/D-scratch): addr[13:2] => 16 KiB, repeats every 16 KiB.
 *   DDR: {4'h0, addr[27:0]} => nibbles 0x1,0x2,0x3 all hit the SAME DDR (0x1 data,
 *        0x2 inst/I-cache, 0x3 both). Modelled as one 256 MiB buffer.
 *   MMIO: MemMapIO addra = addr[13:2] => the 16 KiB map repeats every 0x4000 (addr[27:14]
 *        ignored). The registers answer only when addra[11:6]==0 (0x80000000-0x800000FC,
 *        compared on addra[5:0]); the LCD text buffer is addra[11:5]==2 (0x80000100-0x8000017C);
 *        the rest of the 16 KiB reads 0 here (HW: 32'dx) and drops stores.
 * We reproduce this with a UC_HOOK_TLB_FILL (vaddr -> canonical paddr) in UC_TLB_VIRTUAL mode.
 */
#ifndef VV_MEMMAP_H
#define VV_MEMMAP_H
#include <stdint.h>

enum vv_kind { VV_RAM, VV_MMIO };

typedef struct {
    const char *name;
    uint8_t     nibble;   /* ADDR[31:28] that selects it                     */
    uint32_t    base;     /* canonical physical base we map in Unicorn        */
    uint32_t    size;     /* power of two; alias mask = size-1                */
    uint8_t     prot;     /* UC_PROT_* bits (BIOS is RO for data in HW)       */
    enum vv_kind kind;
} vv_region;

#define VV_KiB 1024u
#define VV_MiB (1024u*1024u)

/* name        nib   base         size          prot(R=1,W=2,X=4) kind */
#define VV_MEMMAP_TABLE \
    X("DDR",   0x1, 0x10000000u, 256*VV_MiB,   7, VV_RAM ) /* also 0x2,0x3 alias */ \
    X("BIOS",  0x4, 0x40000000u,  16*VV_KiB,   7, VV_RAM ) /* HW: data-RO; writes logged */ \
    X("DSCR",  0x5, 0x50000000u,  16*VV_KiB,   7, VV_RAM ) /* D-scratch == SM_BASE_ADDR shared mem */ \
    X("ISCR",  0x6, 0x60000000u,  16*VV_KiB,   7, VV_RAM ) /* I-scratch */ \
    X("MMIO",  0x8, 0x80000000u,  16*VV_KiB,   3, VV_MMIO) /* MemMapIO addra[11:0], aliases every 16 KiB */ \
    X("ISR",   0xC, 0xC0000000u,  16*VV_KiB,   7, VV_RAM ) /* isr_mem; PC_ISR=0xC0000180 */

/* Extra nibbles that alias onto another region's canonical base. */
#define VV_ALIAS_TABLE \
    A(0x2, 0x1) /* I-cache path into DDR */ \
    A(0x3, 0x1) /* both caches into DDR  */

/* Known addresses (mmio_intr_cop0.h / shared6.h / graphics.h) */
#define VV_MMIO_BASE      0x80000000u
#define VV_OW_UATX_READY  0x0000u
#define VV_OW_UARX_VALID  0x0004u
#define VV_OW_UATX_DATA   0x0008u
#define VV_OW_UARX_DATA   0x000Cu
#define VV_OW_CNT_CYCLE   0x0010u
#define VV_OW_CNT_INST    0x0014u
#define VV_OW_CNT_RESET   0x0018u
#define VV_OW_PF_FRAME    0x0050u
#define VV_OW_GP_FRAME    0x0054u
#define VV_OW_GP_GCODE    0x0058u
#define VV_OW_GP_STATE    0x005Cu
#define VV_OW_RANDOM32    0x0080u   /* PRNG32LFSR: read value / write seed (src/prng.c) */
/* 0x84-0xFC: extended area, inside io_base but decoded by no MemMapIO.v case (writes do nothing,
 * reads 32'dx on the FPGA).  vvizard-only registers live here. */
#define VV_OW_GUEST_EXIT  0x0084u   /* store: end the run with that exit code (main.c, --no-guest-exit) */
#define VV_MMIO_DECODE    0x3FFCu   /* word offset within the map: addra[11:0] << 2 */
#define VV_MMIO_REG_SPAN  0x0100u   /* registers: addra[11:6]==0 only (no 0x100 aliases) */
#define VV_OW_LCD_BASE    0x0100u   /* LCD char i at 0x80000100 + 4*i, i = 0..31 (src/lcd.h) */
#define VV_OW_LCD_END     0x0180u
#define VV_SM_BASE_ADDR   0x50000000u   /* shared6.h: plain RAM in D-scratch */
#define VV_GPTEMP         0x12003000u   /* graphics.h GP command buffer (DDR) */
#define VV_FRAME0         0x10000000u   /* frame N at 0x10000000 + N<<FSHIFT */
#define VV_FSHIFT         22
#define VV_FNMASK         0x3Fu
#define VV_PC_BOOT        0x40000000u
#define VV_PC_ISR         0xC0000180u

/* Decode table, one entry per ADDR[31:28] (built by main.c from the two tables above; aliases
 * included): canonical base, alias mask (size-1) and, for RAM, the host buffer Unicorn maps with
 * uc_mem_map_ptr, so host code reads guest RAM without uc_mem_read.  Guest RAM holds the guest's
 * byte order (big-endian by default).  Host WRITES still go through uc_mem_write: it invalidates
 * translated code in the written range, a plain store into the buffer would not. */
typedef struct {
    uint8_t *host;        /* RAM buffer at `base` (NULL: MMIO or undecoded) */
    uint32_t base, mask;  /* canonical base, size-1 */
    uint8_t  ok;          /* decoded nibble */
} vv_nibble;
extern vv_nibble vv_nib[16];
/* Canonical address of a (hardware top-nibble decode), 0xFFFFFFFF if undecoded. */
static inline uint32_t vv_canon_inl(uint32_t a) {
    const vv_nibble *t = &vv_nib[a >> 28];
    return t->ok ? t->base | (a & t->mask) : 0xFFFFFFFFu;
}
/* Host pointer to n bytes of RAM at CANONICAL address c, or NULL when [c, c+n) is not inside one
 * RAM region (MMIO, undecoded, past the end): where uc_mem_read would fail or reach MMIO. */
static inline uint8_t *vv_ram(uint32_t c, uint32_t n) {
    const vv_nibble *t = &vv_nib[c >> 28];
    if (!t->host || (c & ~t->mask) != t->base || (uint64_t)(c & t->mask) + n > (uint64_t)t->mask + 1) return 0;
    return t->host + (c & t->mask);
}
/* Big-endian 32-bit word at canonical c from RAM; *ok = 0 (and 0 returned) when not RAM. */
static inline uint32_t vv_ram_be32(uint32_t c, int *ok) {
    const uint8_t *b = vv_ram(c, 4);
    if (!b) { *ok = 0; return 0; }
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
#endif

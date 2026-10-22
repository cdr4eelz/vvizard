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
 *   MMIO: MemMapIO addra = addr[13:2], decodes only addra[5:0] => repeats every 0x100.
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
    X("MMIO",  0x8, 0x80000000u,   4*VV_KiB,   3, VV_MMIO) /* MemMapIO, aliases every 0x100 */ \
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
#define VV_SM_BASE_ADDR   0x50000000u   /* shared6.h: plain RAM in D-scratch */
#define VV_GPTEMP         0x12003000u   /* graphics.h GP command buffer (DDR) */
#define VV_FRAME0         0x10000000u   /* frame N at 0x10000000 + N<<FSHIFT */
#define VV_FSHIFT         22
#define VV_FNMASK         0x3Fu
#define VV_PC_BOOT        0x40000000u
#define VV_PC_ISR         0xC0000180u
#endif

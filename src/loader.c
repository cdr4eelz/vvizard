/* loader.c - raw binary and minimal ELF32 PT_LOAD loader (written from scratch). */
#include "term.h"
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); *len = ftell(f); rewind(f);
    unsigned char *b = malloc(*len ? *len : 1);
    if (b && fread(b, 1, *len, f) != (size_t)*len) { free(b); b = NULL; }
    fclose(f); return b;
}
int vv_is_elf(const char *path) {
    unsigned char m[4] = {0}; FILE *f = fopen(path, "rb"); if (!f) return 0;
    size_t n = fread(m, 1, 4, f); fclose(f);
    return n == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
}
int vv_load_raw(uc_engine *uc, const char *path, uint32_t addr) {
    long len; unsigned char *b = slurp(path, &len); if (!b) return -1;
    uc_err e = uc_mem_write(uc, vv_canon(addr), b, len); free(b);
    if (e) { vv_eprintf("load %s@0x%08x: %s\n", path, addr, uc_strerror(e)); return -1; }
    vv_eprintf("[load] raw %s -> 0x%08x (%ld bytes)\n", path, addr, len);
    return 0;
}
static uint32_t rd32(const unsigned char *p, int be) {
    return be ? (uint32_t)p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3]
              : (uint32_t)p[3]<<24 | p[2]<<16 | p[1]<<8 | p[0];
}
static uint16_t rd16(const unsigned char *p, int be) {
    return be ? (uint16_t)(p[0]<<8 | p[1]) : (uint16_t)(p[1]<<8 | p[0]);
}
int vv_load_elf(uc_engine *uc, const char *path, uint32_t *entry, int big_endian) {
    long len; unsigned char *b = slurp(path, &len); if (!b) return -1;
    int rc = -1;
    if (len < 52 || b[4] != 1 /*ELFCLASS32*/) { vv_eprintf("%s: not ELF32\n", path); goto out; }
    int be = (b[5] == 2);
    if (be != !!big_endian) vv_eprintf("[load] warning: ELF endianness (%s) != CPU mode\n", be ? "BE" : "LE");
    if (rd16(b + 18, be) != 8) vv_eprintf("[load] warning: e_machine != EM_MIPS\n");
    *entry = rd32(b + 24, be);
    uint32_t phoff = rd32(b + 28, be);
    uint16_t phentsize = rd16(b + 42, be), phnum = rd16(b + 44, be);
    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *ph = b + phoff + i * phentsize;
        if (ph + 32 > b + len) break;
        if (rd32(ph, be) != 1 /*PT_LOAD*/) continue;
        uint32_t off = rd32(ph+4, be), paddr = rd32(ph+12, be), fsz = rd32(ph+16, be), msz = rd32(ph+20, be);
        if (msz == 0) continue;
        if ((long)off + fsz > len || vv_canon(paddr) == 0xFFFFFFFFu) {
            vv_eprintf("[load] skip PT_LOAD paddr 0x%08x filesz 0x%x (outside file or undecoded nibble)\n", paddr, fsz);
            continue;
        }
        if (fsz && uc_mem_write(uc, vv_canon(paddr), b + off, fsz)) { vv_eprintf("[load] cannot write 0x%08x\n", paddr); goto out; }
        if (msz > fsz) { void *z = calloc(1, msz - fsz); uc_mem_write(uc, vv_canon(paddr + fsz), z, msz - fsz); free(z); }
        vv_eprintf("[load] PT_LOAD 0x%08x filesz 0x%x memsz 0x%x\n", paddr, fsz, msz);
    }
    vv_eprintf("[load] ELF %s entry 0x%08x\n", path, *entry);
    rc = 0;
out: free(b); return rc;
}

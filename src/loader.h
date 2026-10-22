#ifndef VV_LOADER_H
#define VV_LOADER_H
#include <stdint.h>
#include <unicorn/unicorn.h>
int vv_load_raw(uc_engine *uc, const char *path, uint32_t addr);
/* returns 0 ok; *entry set from e_entry; big_endian chooses header byte order */
int vv_load_elf(uc_engine *uc, const char *path, uint32_t *entry, int big_endian);
/* provided by main.c: alias address -> canonical mapped address */
uint32_t vv_canon(uint32_t addr);
int vv_is_elf(const char *path);
#endif

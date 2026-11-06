/* rtl.h - optional approximate cache-stall runner.  Compiled only with -DVV_FIDELITY_RTL
 * (make FIDELITY=rtl).  The default and --cpu=fast binaries do not include this file. */
#ifndef VV_RTL_H
#define VV_RTL_H
#include <stdint.h>
#include <unicorn/unicorn.h>

uc_err vv_rtl_run(uc_engine *uc, uint32_t entry, uint32_t until, int has_until);
#endif

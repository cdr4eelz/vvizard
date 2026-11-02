/* types_guard.h - lets a HOST translation unit include the vendored cs150 headers
 * (vendor/cs150/software/common/mmio_intr_cop0.h, .../proj_app7/shared7.h).
 *
 * Those headers #include "types.h", which typedefs uint32_t/size_t/... and #defines NULL; a host TU
 * already has <stdint.h>, and a second typedef is an error.  Defining types.h's include guard first
 * turns that include into a no-op, and this file supplies the few names the headers use instead.
 * The vendored types.h itself is still compiled, alone, by types_check.c.
 * When vendor/cs150/software/common/types.h changes, re-read its diff and adjust this file. */
#ifndef VV_TYPES_GUARD_H
#define VV_TYPES_GUARD_H
#include <stdint.h>
#include <stddef.h>
#ifndef TYPES_H_
#define TYPES_H_
#endif
typedef int BOOL;
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef TRUE
#define TRUE  (1)
#define FALSE (0)
#endif
#define sizeof_member(T, m)    (sizeof(((T *)0)->m))
#define offset_of_member(T, m) (__builtin_offsetof(T, m))
#endif

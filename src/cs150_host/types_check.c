/* types_check.c - compiles the vendored cs150 types.h ALONE (no system headers, so its typedefs and
 * NULL do not collide) on every build.  types_guard.h hides its body from cs150_check.c; this keeps
 * that body compiling and checks the widths struct SM_DATA relies on.  Built, not linked. */
#include "types.h"

_Static_assert(sizeof(uint32_t) == 4 && sizeof(int32_t) == 4, "types.h 32-bit");
_Static_assert(sizeof(uint16_t) == 2 && sizeof(int16_t) == 2, "types.h 16-bit");
_Static_assert(sizeof(uint8_t) == 1 && sizeof(int8_t) == 1, "types.h 8-bit");
_Static_assert((int8_t)-1 < 0 && (int16_t)-1 < 0 && (uint16_t)-1 > 0, "types.h signedness");
_Static_assert(HILO16(0x1234, 0x5678) == 0x12345678 && HI16(0x12345678) == 0x1234 && LO8(0x1234) == 0x34,
               "types.h HILO/HI/LO helpers");

struct vv_types_probe { uint8_t a; uint32_t b; };
_Static_assert(offset_of_member(struct vv_types_probe, b) == 4, "types.h offset_of_member");
_Static_assert(sizeof_member(struct vv_types_probe, b) == 4, "types.h sizeof_member");
_Static_assert(TRUE == 1 && FALSE == 0, "types.h TRUE/FALSE");

typedef BOOL vv_types_check_tu;

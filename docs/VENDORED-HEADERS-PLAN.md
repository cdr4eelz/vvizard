# Plan: vendored cs150 headers, compared with the originals when visible

Status: **plan only, not implemented** (Nov 1 2026). Drafted by Grok Build from the vvizard tree at commit 55ef744
(proj7 sync) with the cs150 ISE tree beside it; reviewed and amended by Grok Bot. The amendments are listed at the end.

**Goal.** vvizard keeps its own copies of `software/common/mmio_intr_cop0.h` and `software/proj_app7/shared7.h` (plus
what they need) and builds only against those, so a standalone clone builds anywhere. When the cs150 originals are
visible at the relative path (`../../software/...` from `extern/vvizard`), the build compares them with the copies and
warns, or fails if asked, on any difference.

**Summary.** vvizard keeps building against byte-for-byte snapshots under `vendor/cs150/`. Every `make` compiles `src/cs150_host/cs150_check.c`, which `_Static_assert`s the `VV_*` constants and the `struct SM_DATA` layout against those snapshots. Every `make` also byte-compares the snapshots with the originals when they are visible (one warning line per differing file; review amendment 1), and `make check-vendor` (a dependency of `make test`) shows the diff and the semantic `.h`/`.s.inc`/`hw.inc` checks. `VENDOR_CHECK` defaults to `warn`.

## Decisions

**Vendor these five files, unchanged.** No banner inside them, so `cmp` can be byte-identical. `types.h` is included because both headers `#include "types.h"`. The two `.s.inc` files are included because they are compared too.

| Vendored path | Upstream, relative to the vvizard directory |
|---|---|
| `vendor/cs150/software/common/types.h` | `../../software/common/types.h` |
| `vendor/cs150/software/common/mmio_intr_cop0.h` | `../../software/common/mmio_intr_cop0.h` |
| `vendor/cs150/software/common/mmio_intr_cop0.s.inc` | `../../software/common/mmio_intr_cop0.s.inc` |
| `vendor/cs150/software/proj_app7/shared7.h` | `../../software/proj_app7/shared7.h` |
| `vendor/cs150/software/proj_app7/shared7.s.inc` | `../../software/proj_app7/shared7.s.inc` |

Sidecars, not copies of upstream files:

- `vendor/cs150/PROVENANCE` is rewritten by `make vendor-update`.
- `vendor/cs150/known-drift` is hand-maintained. `vendor-update` must not touch it.

`PROVENANCE` fields:

```
source_root: ../../software
source_commit: <git rev-parse HEAD, or "unknown">
source_commit_date: <git log -1 --format=%cI, or "unknown">
source_dirty: yes|no|unknown
vendor_date: <UTC YYYY-MM-DDThh:mm:ssZ>
files:
  software/common/types.h sha256:<hex>
  software/common/mmio_intr_cop0.h sha256:<hex>
  software/common/mmio_intr_cop0.s.inc sha256:<hex>
  software/proj_app7/shared7.h sha256:<hex>
  software/proj_app7/shared7.s.inc sha256:<hex>
```

Commit lookup is `git -C $(CS150_SW) rev-parse HEAD`. That works when `software/` sits inside a cs150 clone (the normal `cs150/extern/vvizard` layout on UBU24). A tarball or copied tree has no git: record `unknown` and still copy. `source_dirty` is `git -C ../../software status --porcelain -- <those five paths>`.

**Consume the headers from one host check translation unit, through a types guard. Do not include them from `memmap.h`, `hw.h`, `shmem.c`, or any device file.**

`mmio_intr_cop0.h` sits next to `types.h`, and `#include "types.h"` searches that directory first, so a `-I` shim cannot outrank the sibling. `types.h` also `typedef`s `uint32_t` and `size_t` and `#define`s `NULL`. Every vvizard TU already includes `<stdint.h>` via `memmap.h` / `shmem.h`, and a second typedef is a compile error. `HALT()`, `ISR_STATUS()`, and `ISR_COMPARE()` are MIPS inline asm; they are harmless only while nothing invokes them. `shared7.h` declares `SM_INIT7`, `uwrite_*`, and `tbuff`, and `#define SM_BASE` casts `0x50000000` to `struct SM_DATA *`. Guest RAM is big-endian (`vv_ram_be32` in `src/memmap.h`); a host `struct SM_DATA` has the same offsets and the wrong byte order. `shared6.h` defines another `struct SM_DATA` and another `K_MAGIC_VERSION`, so it cannot be included in that same TU.

`src/cs150_host/types_guard.h` defines the include guard and the macros the headers actually use, then the real `#include "types.h"` becomes a no-op:

```c
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
#define TRUE 1
#define FALSE 0
#endif
#define sizeof_member(T, m) (sizeof(((T *)0)->m))
#define offset_of_member(T, m) (__builtin_offsetof(T, m))
```

`src/cs150_host/cs150_check.c` (not `src/*.c`, so `SRCS := $(wildcard src/*.c)` does not link it into `vvizard`):

```c
#include "types_guard.h"
#include "mmio_intr_cop0.h"
#include "shared7.h"
#include "memmap.h"
#include "hw.h"
#include "shmem.h"

#define VV_EQ(a, b) _Static_assert((unsigned long)(a) == (unsigned long)(b), #a " == " #b)

/* MMIO offsets: src/memmap.h VV_OW_*  vs  OW_* */
VV_EQ(VV_MMIO_BASE,     MMIO_BASE);
VV_EQ(VV_OW_UATX_READY, OW_UATX_READY);
VV_EQ(VV_OW_UARX_VALID, OW_UARX_VALID);
VV_EQ(VV_OW_UATX_DATA,  OW_UATX_DATA);
VV_EQ(VV_OW_UARX_DATA,  OW_UARX_DATA);
VV_EQ(VV_OW_CNT_CYCLE,  OW_CNT_CYCLE);
VV_EQ(VV_OW_CNT_INST,   OW_CNT_INST);
VV_EQ(VV_OW_CNT_RESET,  OW_CNT_RESET);
VV_EQ(VV_OW_PF_FRAME,   OW_PF_FRAME);
VV_EQ(VV_OW_GP_FRAME,   OW_GP_FRAME);
VV_EQ(VV_OW_GP_GCODE,   OW_GP_GCODE);
VV_EQ(VV_OW_GP_STATE,   OW_GP_STATE);
VV_EQ(VV_OW_RANDOM32,   OW_RANDOM32);
VV_EQ(VV_OW_HALTVAL,    OW_HALTVAL);
VV_EQ(VV_SM_BASE_ADDR,  SM_BASE_ADDR);

/* Cause/Status masks: src/hw.h VV_IP_*  vs  IM_*  (names differ: GP/PF vs GPU/FRAME) */
VV_EQ(VV_IP_UARX,  IM_UARX);
VV_EQ(VV_IP_UATX,  IM_UATX);
VV_EQ(VV_IP_GP,    IM_GPU);
VV_EQ(VV_IP_PF,    IM_FRAME);
VV_EQ(VV_IP_RTC,   IM_RTC);
VV_EQ(VV_IP_TIMER, IM_TIMER);
VV_EQ(VV_IP_ALL,   IM_POSSIBLE);
VV_EQ(VV_CPU_HZ,   K_CPU_HZ);

/* src/shmem.h proj7 offsets and stamp vs shared7.h. VV_SM6_* stay manual. */
VV_EQ(VV_SM7_MAGIC,          K_MAGIC_VERSION);
VV_EQ(VV_SM7_MAJOR,          K_MAJOR_VERSION);
VV_EQ(VV_SM_BUFSZ,           K_SHBUF_SIZEB);
VV_EQ(VV_SM7_magic1,         SMO_magic1);
VV_EQ(VV_SM7_majorvers,      SMO_majorvers);
VV_EQ(VV_SM7_stash0,         SMO_stash0);
VV_EQ(VV_SM7_state,          SMO_state);
VV_EQ(VV_SM7_flags,          SMO_flags);
VV_EQ(VV_SM7_RTC_count,      SMO_RTC_count);
VV_EQ(VV_SM7_seconds,        SMO_seconds);
VV_EQ(VV_SM7_clock,          SMO_clock);
VV_EQ(VV_SM7_player_y,       SMO_player_y);
VV_EQ(VV_SM7_player_x,       SMO_player_x);
VV_EQ(VV_SM7_max_opponents,  SMO_max_opponents);
VV_EQ(VV_SM7_num_opponents,  SMO_num_opponents);
VV_EQ(VV_SM7_buff_size,      SMO_buff_size);
VV_EQ(VV_SM7_buff_head,      SMO_buff_head);
VV_EQ(VV_SM7_buff_tail,      SMO_buff_tail);
VV_EQ(VV_SM7_buff_data,      SMO_buff_data);
VV_EQ(VV_SM7_magic2,         SMO_magic2);
VV_EQ(VV_SM7_PAST_END,       SMO_PAST_END);
VV_EQ(VV_SM_FLAG_PAUSE,      SMM_flags_pause);
VV_EQ(VV_SM_FLAG_NOTIMER,    SMM_flags_notimer);
VV_EQ(VV_SM_FLAG_QUIT,       SMM_flags_quit);

/* shared7.c SM_CHECK_OFFSET, written as _Static_assert. Layout only. */
#define VV_OFF(member, expected) \
    _Static_assert(__builtin_offsetof(struct SM_DATA, member) == (expected), #member)
VV_OFF(magic1, SMO_magic1);
VV_OFF(majorvers, SMO_majorvers);
VV_OFF(stash0, SMO_stash0);
VV_OFF(stash1, SMO_stash1);
VV_OFF(stash2, SMO_stash2);
VV_OFF(stash3, SMO_stash3);
VV_OFF(state, SMO_state);
VV_OFF(flags, SMO_flags);
VV_OFF(RTC_count, SMO_RTC_count);
VV_OFF(seconds, SMO_seconds);
VV_OFF(clock, SMO_clock);
VV_OFF(player_y, SMO_player_y);
VV_OFF(player_x, SMO_player_x);
VV_OFF(max_opponents, SMO_max_opponents);
VV_OFF(num_opponents, SMO_num_opponents);
VV_OFF(buff_size, SMO_buff_size);
VV_OFF(buff_head, SMO_buff_head);
VV_OFF(buff_tail, SMO_buff_tail);
VV_OFF(buff_data, SMO_buff_data);
VV_OFF(magic2, SMO_magic2);
_Static_assert(sizeof(((struct SM_DATA *)0)->buff_data) == K_SHBUF_SIZEB, "buff_data");
_Static_assert(__builtin_offsetof(struct SM_DATA, magic2) ==
               __builtin_offsetof(struct SM_DATA, buff_data) + K_SHBUF_SIZEB, "magic2");
_Static_assert(sizeof(struct SM_DATA) == SMO_PAST_END, "sizeof SM_DATA");
_Static_assert(sizeof(struct SM_DATA) == VV_SM7_PAST_END, "sizeof vs VV_SM7_PAST_END");
_Static_assert(sizeof(uint32_t) == 4 && sizeof(uint16_t) == 2 && sizeof(int8_t) == 1, "widths");

#undef HALT
#undef ISR_STATUS
#undef ISR_COMPARE
#undef SM_BASE
#define HALT(v)           VV_DO_NOT_CALL_MIPS_ASM_HALT
#define ISR_STATUS(k, s)  VV_DO_NOT_CALL_MIPS_ASM_ISR_STATUS
#define ISR_COMPARE(c)    VV_DO_NOT_CALL_MIPS_ASM_ISR_COMPARE
```

`_Static_assert` is a GNU extension accepted by gcc and clang at the Makefile’s `-std=gnu99`.

**The guard hides the vendored `types.h` body**, so add a second tiny TU, `src/cs150_host/types_check.c`, that includes
nothing but the vendored `types.h` (no system headers, so its `typedef`s and `NULL` do not collide) and asserts the widths
it promises: `_Static_assert(sizeof(uint32_t) == 4 && sizeof(uint16_t) == 2 && sizeof(int16_t) == 2 && sizeof(int8_t) == 1, "types.h")`,
plus `offset_of_member` / `sizeof_member` used once. Compile it in the same rule family (`$(BUILD_DIR)/types_check.o`,
also a `$(TARGET)` prerequisite, not linked). That keeps the real `types.h` compiled on every build. The file has no `main` and is not linked. Guest memory stays on the existing `vv_sm_reader` path in `src/shmem.c`. `SM_DATA` has no pointer members (`0x140` bytes), so its host offsets equal the MIPS o32 ones on any host; no pointer-size assert (it would only break 32-bit hosts). If a pointer member ever appears, the `VV_OFF` asserts against the `SMO_*` constants catch the mismatch on 64-bit hosts.

`VV_*` stay as the literals in `src/memmap.h`, `src/hw.h`, and `src/shmem.h`. The check TU is what binds them. `VV_SM6_*` are not checked: `shared6.h` is out of scope for this change, and including it would collide with `struct SM_DATA`.

## Makefile

```make
VENDOR_CHECK ?= warn
ifeq ($(filter warn fail off,$(VENDOR_CHECK)),)
$(error VENDOR_CHECK must be warn, fail, or off (got '$(VENDOR_CHECK)'))
endif

CS150_SW    ?= ../../software   # override: make CS150_SW=/path/to/cs150/software
VENDOR_ROOT := vendor/cs150
VENDOR_FILES := \
    software/common/types.h \
    software/common/mmio_intr_cop0.h \
    software/common/mmio_intr_cop0.s.inc \
    software/proj_app7/shared7.h \
    software/proj_app7/shared7.s.inc

CS150_CHECK_SRC := src/cs150_host/cs150_check.c
CS150_CHECK_OBJ := $(BUILD_DIR)/cs150_check.o

$(CS150_CHECK_OBJ): $(CS150_CHECK_SRC) src/cs150_host/types_guard.h $(HDRS) $(FLAGS_STAMP) \
        $(addprefix $(VENDOR_ROOT)/,$(VENDOR_FILES)) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc/cs150_host \
	    -I$(VENDOR_ROOT)/software/common -I$(VENDOR_ROOT)/software/proj_app7 \
	    $(UNICORN_CFLAGS) $(CFLAGS) -c $(CS150_CHECK_SRC) -o $@

$(TARGET): $(OBJS) $(FLAGS_STAMP) $(CS150_CHECK_OBJ)

.PHONY: check-vendor vendor-update test-vendor

# Every build: a byte compare (POSIX cmp, no python) whenever a vendored or visible upstream file
# changed.  Missing upstream tree/file -> silent skip; VENDOR_CHECK=fail makes a difference fatal.
VENDOR_UP := $(wildcard $(addprefix $(CS150_SW)/../,$(VENDOR_FILES)))
VENDOR_STAMP := $(BUILD_DIR)/.vendor-cmp
$(VENDOR_STAMP): $(addprefix $(VENDOR_ROOT)/,$(VENDOR_FILES)) $(VENDOR_UP) | $(BUILD_DIR)
	@bad=0; [ "$(VENDOR_CHECK)" = off ] || for f in $(VENDOR_FILES); do u=$(CS150_SW)/../$$f; \
	  [ -f "$$u" ] || continue; cmp -s "$(VENDOR_ROOT)/$$f" "$$u" || { \
	  echo "vendor: WARNING $$f differs from $$u (make check-vendor shows the diff)"; bad=1; }; done; \
	if [ $$bad = 1 ] && [ "$(VENDOR_CHECK)" = fail ]; then exit 1; fi; touch $@
$(TARGET): $(VENDOR_STAMP)

check-vendor:
	python3 scripts/check_vendor.py --mode $(VENDOR_CHECK) --cc "$(CC)" \
	    --vendor $(VENDOR_ROOT) --upstream $(CS150_SW) \
	    --hw-inc tests/hw/hw.inc

vendor-update:
	python3 scripts/check_vendor.py --update --cc "$(CC)" \
	    --vendor $(VENDOR_ROOT) --upstream $(CS150_SW)

test-vendor:
	python3 tests/test_vendor.py

test: check-vendor check-hex test-vendor test-hex test-hw test-quirks test-player test-sm test-threads
```

Add `check-vendor`, `vendor-update`, and `test-vendor` to the `.PHONY` line. `CPPFLAGS` stays `-I$(SRC_DIR)` only. The vendor `-I` paths are on this one recipe, with `-Isrc/cs150_host` first. `types.h` next to `mmio_intr_cop0.h` is still found first by that header; `TYPES_H_` makes the body a no-op.

`all` does not depend on the full `check-vendor` (diff, semantic `.h`/`.s.inc` comparison, `hw.inc`), which forks a host compiler and is what `make test` and CI should show. But every build does two cheap things: it compiles `cs150_check.o` (snapshot vs `VV_*`), and the `$(VENDOR_STAMP)` rule byte-compares the vendored files with the originals when they are visible, printing one warning line per differing file (fatal with `VENDOR_CHECK=fail`). The stamp depends on the upstream files through `$(wildcard ...)`, so it re-runs when the cs150 copies change and is a no-op when they are absent. Note `$(CS150_SW)/../` is the cs150 root, so `$(CS150_SW)/../software/common/...` is the original. (With the warning the stamp is still touched, so a `warn` is printed once per change, not on every `make`.) `$(TARGET)` depends on `cs150_check.o`, so a bad snapshot fails `make`, `make test`, and `make run`.

`VENDOR_CHECK=off` makes `check-vendor` print `check-vendor: skipped (VENDOR_CHECK=off)` and exit 0. It does not skip `cs150_check.o`.

**Default is `warn`.** `NOTES.md` already records that this tree is developed with uncommitted edits to `mmio_intr_cop0.h`. Those edits should show up as a diff, and they should not turn `make test` red while the headers are in progress. Standalone clones have no originals; the compile-time asserts still protect the snapshot. `VENDOR_CHECK=fail` is the strict switch for a CI job that has the cs150 tree and wants drift to fail the suite.

## `scripts/check_vendor.py`

One Python 3 script. Byte compare and the unified diff are `difflib.unified_diff` on the raw bytes decoded as latin-1, so GNU `diff` vs macOS `diff`, and GNU `sed` vs BSD `sed`, are not involved. No whitespace or newline normalization. The failure message includes the hand command `diff -u vendor/cs150/<rel> ../../software/<rel>`.

Modes:

| Situation | `warn` | `fail` | `off` |
|---|---|---|---|
| `../../software` is not a directory | one skip line, exit 0 | exit 0 | exit 0 |
| `../../software` exists, a listed file is missing | name it, exit 0 | exit 1 | exit 0 |
| bytes differ | print the diff, exit 0 | print the diff, exit 1 | exit 0 |
| bytes match, `PROVENANCE` commit ≠ current HEAD | one note, exit 0 | one note, exit 0 | exit 0 |
| semantic mismatch (below) | print it, exit 0 | exit 1 | exit 0 |
| known-drift pair, values exactly as recorded | always print the report line, do not fail | same | skip the report |

A missing tree is a skip even under `fail`, so a standalone CI job can export `VENDOR_CHECK=fail` and stay green. A present tree with a missing file is a partial tree and follows the mode.

When bytes differ and `git -C ../../software` works, add one annotation line: if `git show HEAD:<file>` equals the vendored bytes, say the working tree has uncommitted edits and the snapshot still matches HEAD. If HEAD also differs, say both HEAD and the working tree differ. The annotation does not change the exit code.

**`.h` vs `.s.inc` values.** `gcc -E -dM` prints unevaluated bodies (`(0x0084u)` vs `0x0084`, `(K_SHBUF_SIZEB - 1)` vs `0x00FF`), so the script compiles and runs a tiny host printer into `build/vendor-check/`.

- For each `.h`: a `print.c` that includes `types_guard.h` and then the header, and `printf`s `(unsigned long)<NAME>` for every object-like macro whose body matches `[0-9A-Za-z_() +\-<>&|~*]`. That skips `HALT`, `ISR_STATUS`, `ISR_COMPARE`, `COP0_*` (they contain `"$9"`), and `SM_BASE`.
- For each `.s.inc`: rewrite `.equiv NAME, EXPR` to `#define NAME (EXPR)`, leave `#` comments as `#` comments, then print the same way.
- Compare the intersection. Also pair `SM_BASE_ADDR` (header) with `SM_BASE` (asm). Parse `COP0_Count` and friends as `"$N"` vs `$N` and compare `N`.
- `II_*` and `SMI_*` are compared too, even though vvizard has no `VV_*` for them. Cast through `(unsigned long)` so `(uint16_t)(~1)` and `~1 & 0xFFFF` both print `0xfffe`.

`tests/hw/hw.inc` literals, checked against the printed `OW_*` / `MMIO_BASE` (not against a generated include):

| `hw.inc` | vendored macro |
|---|---|
| `IO` `0x8000` | `MMIO_BASE >> 16` |
| `TX_READY` | `OW_UATX_READY` |
| `RX_VALID` | `OW_UARX_VALID` |
| `TX_DATA` | `OW_UATX_DATA` |
| `RX_DATA` | `OW_UARX_DATA` |
| `PF_FRAME` | `OW_PF_FRAME` |
| `GP_FRAME` | `OW_GP_FRAME` |
| `GP_CODE` | `OW_GP_GCODE` |
| `GP_STATE` | `OW_GP_STATE` |
| `RANDOM32` | `OW_RANDOM32` |
| `HALTVAL` | `OW_HALTVAL` |

**Known intentional divergence.** Today `shared7.h` has `K_MAGIC_VERSION (0xFEDBEEF3u)` and `K_MAJOR_VERSION 7`. `shared7.s.inc` has `0xFEDBEEF2` and `6`. Offsets match. isr7 `DO_INIT_VERSION` stamps the asm constants; `src/shmem.c` already decodes that as proj7 and warns. `vendor/cs150/known-drift`:

```
# macro  header  asm
# Report this pair. Do not fail the build while the values stay exactly these.
# If they become equal, or change to any other pair, the exception does not apply.
K_MAGIC_VERSION  software/proj_app7/shared7.h=0xFEDBEEF3  software/proj_app7/shared7.s.inc=0xFEDBEEF2
K_MAJOR_VERSION  software/proj_app7/shared7.h=7          software/proj_app7/shared7.s.inc=6
```

- Exact recorded pair: print `check-vendor: known drift K_MAGIC_VERSION shared7.h=0xFEDBEEF3 shared7.s.inc=0xFEDBEEF2 (intentional; isr7 stamp)` and do not count it as a failure in either mode.
- Any other pair: the exception does not apply. Print `does not match known-drift`, and follow `VENDOR_CHECK`.
- Both sides equal: print `known-drift K_MAGIC_VERSION no longer differs; update vendor/cs150/known-drift, README "Shared memory", and the src/shmem.c warning text`, and follow `VENDOR_CHECK`. The emulator is still consistent; the allowlist and the docs are stale.

Any other macro that differs between a `.h` and its `.s.inc` (a `SMO_*` offset, an `OW_*`, an `IM_*`) follows `VENDOR_CHECK` with no exception. That is the case that would make `src/shmem.c` mis-read a field.

`--update` requires every upstream file to exist (missing source is exit 1, unlike the check). It `shutil.copyfile`s bytes with no newline conversion, rewrites `PROVENANCE`, leaves `known-drift` alone, and prints `vendor-update: copied 5 files from a dirty tree` when `source_dirty` is yes. It does not delete extra files under `vendor/cs150/`.

## Tests

`tests/test_vendor.py` is hermetic. It copies `vendor/cs150` into a temp directory and points the script at temp upstream trees. It does not read `../../software`. Cases:

- Identical trees, `warn` and `fail`: exit 0, and the known-drift line for `0xFEDBEEF3` / `0xFEDBEEF2` is present.
- Flip one byte of upstream `mmio_intr_cop0.h`: `warn` exits 0 and the diff names `OW_` or the flipped text; `fail` exits 1.
- Delete one upstream file while leaving `software/` in place: `warn` exits 0, `fail` exits 1, output contains `partial`.
- No `software/` directory: `warn` and `fail` both exit 0 and say `skipped`.
- Upstream `shared7.s.inc` magic changed to `0xFEDBEEF0`: not the recorded pair; `fail` exits 1.
- Upstream `shared7.s.inc` magic changed to `0xFEDBEEF3` and major to `7`: output says the known-drift entry is stale; `fail` exits 1; `warn` exits 0.
- A temp `hw.inc` with `HALTVAL, 0x88`: `fail` exits 1.
- CRLF-only upstream copy: diff output says line endings differ, and the mode’s exit code applies. The script does not strip `\r`.
- Printer values on the real vendored pair: `SMO_magic2 == 0x13C`, `IM_UARX == 0x400`, `IM_POSSIBLE == 0xFC00`, `K_TIMER_CYC == 50000000`, `SM_BASE == SM_BASE_ADDR == 0x50000000`.

`tests/test_sm.py`: replace the hardcoded `H7 = (0xFEDBEEF3, 7)` with a parse of `vendor/cs150/software/proj_app7/shared7.h`. Keep `s_inc_stamp()` reading the live `../../software/proj_app7/shared7.s.inc`, because that is what `isr7.bin` was built from. The stamp warning path already handles “live asm differs from the expected header values.”

No new emulator test for endianness. `tests/hw/sm_proj6.s` and `sm_magic2.s` are assembled `-EB`, and `test_sm.py` already checks the decoded numbers. The check TU never loads a `struct SM_DATA`.

## Docs

`README.md`, in the Build section, next to the `make` list:

- `make check-vendor` compares `vendor/cs150/` with `../../software/` when that tree exists. Standalone clones skip it.
- `VENDOR_CHECK=warn` (default), `fail`, or `off`.
- `make vendor-update` recopies the five files and rewrites `PROVENANCE`. It refuses to run without the upstream files.
- `make test` runs `check-vendor` first.
- The emulator compiles against the vendored copies. `src/cs150_host/cs150_check.c` fails the build if `VV_*` or the proj7 offsets drift from that snapshot.

`README.md` “Shared memory”: say the proj7 column is `vendor/cs150/software/proj_app7/shared7.h`, and that the stale `shared7.s.inc` pair is `vendor/cs150/known-drift`. Keep the current description of the isr7 stamp (0xFEDBEEF2 / 6) and of `SM_INIT7` (0xFEDBEEF3 / 7). Add one sentence: vvizard reads guest memory with explicit big-endian loads at the `VV_SM7_*` offsets and does not overlay `struct SM_DATA` on the D-scratch buffer.

`NOTES.md`, a short subsection after “proj7 sync”: snapshot date, the commit recorded in `PROVENANCE` (or `unknown` if the parent has no git), the five paths, and that `shared6.h` is still hand-copied in `src/shmem.h`.

Comment-only touches, one line each: `src/memmap.h` (top comment: values checked in `cs150_check.c`), `src/hw.h` (the `VV_IP_*` block), `src/shmem.h` (proj7 offsets checked against the vendored header; guest is big-endian), `tests/hw/hw.inc` (literals checked by `scripts/check_vendor.py`).

## Out of scope

`shared6.h`, `shared6.s.inc`, and `graphics.h` (`VV_GPTEMP`, `VV_FRAME0`, `VV_FSHIFT`) stay as they are. Vendoring `shared6.h` needs a second TU; it cannot share `cs150_check.c` with `shared7.h`.

## Step order

1. Copy the five upstream files into `vendor/cs150/...` with `shutil.copyfile` (no edit). Write `PROVENANCE` and `known-drift`.
2. Add `src/cs150_host/types_guard.h` and `src/cs150_host/cs150_check.c`.
3. Add the `cs150_check.o` rule and the `$(TARGET)` dependency. `make` must succeed at the default `-O3`. Touch `vendor/.../shared7.h` and confirm only `cs150_check.o` rebuilds.
4. Add `scripts/check_vendor.py` (`--mode`, `--update`, printer, `hw.inc`, known-drift).
5. Wire `check-vendor`, `vendor-update`, `VENDOR_CHECK`, and `test: check-vendor ... test-vendor ...`.
6. Add `tests/test_vendor.py` and point `test_sm.py`’s `H7` at the vendored header.
7. Update `README.md`, `NOTES.md`, and the four one-line comments.
8. Run `make`, `python3 tests/test_vendor.py`, and `make check-vendor`. With this tree present, the byte compare should be clean on the day of the copy, and the known-drift lines for magic and major should print.

## Risks

- **Originals missing.** `../../software` absent: `check-vendor` skips, exit 0, including under `fail`. `vendor-update` exits 1. `make` still compiles `cs150_check.o` from the snapshot. `test_sm` / `test_player` already need the live tree for `isr7.bin` and GIOS; that is unchanged.
- **Partial tree.** `software/` exists but one of the five files does not (renamed upstream). Named in the output. `fail` exits 1, `warn` exits 0.
- **Uncommitted cs150 edits.** Byte diff plus the HEAD annotation. Default `warn` so a dirty `mmio_intr_cop0.h` does not fail `make test`. `PROVENANCE`’s `source_dirty: yes` records that the snapshot itself was taken dirty.
- **Commit hash only.** Identical bytes with a new HEAD (the header change was committed after `vendor-update`, or a commit did not touch these files) prints a note and does not fail. Content is the gate. Re-run `vendor-update` to refresh the hash.
- **CRLF.** No stripping. A CRLF upstream fails the byte compare until `vendor-update` copies those exact bytes. The message says the mismatch is line endings when a CR strip would make them equal.
- **macOS.** No `sed -i`, no GNU `diff --label`, no `date -Iseconds`, no `readlink -f`. Date format in `PROVENANCE` is `date -u +%Y-%m-%dT%H:%M:%SZ` only if the script shells out; prefer `datetime.now(timezone.utc)`. `cmp`/`diff` behavior is implemented in Python. The printer uses `$(CC)` as passed by make (`CC := gcc`); clang `-std=gnu99 -E` and `_Static_assert` are the same if `CC` is overridden.
- **`types.h` drift.** The guard hides the vendored `types.h` body. A new macro that `shared7.h` starts using fails `cs150_check.o` loudly, or silently uses the guard’s version. The byte compare still flags `types.h`. Updating the snapshot includes reading the `types.h` diff and adjusting `types_guard.h` in the same change. `SM_DATA` uses only `uint32_t`, `uint16_t`, `int16_t`, and `int8_t`, whose widths match MIPS o32 and LP64.
- **Host endianness.** Offsets match across endianness; values do not. A future patch that `memcpy`s D-scratch into `struct SM_DATA`, or that dereferences `SM_BASE`, reads swapped `magic1`. The check TU `#undef`s `SM_BASE`. `shmem.c` keeps using the reader callback.
- **Known-drift goes stale quietly under `warn`.** The “no longer differs” line is easy to miss. `VENDOR_CHECK=fail` is what turns it into a red `make test`. The recorded pair itself never fails the build, which is the requirement.
- **`hw.inc` `IO` is a `lui` immediate**, not a byte offset. The check compares it to `MMIO_BASE >> 16` (`0x8000`), not to `MMIO_BASE`.
- **Printer macros that are not integer constant expressions** fail that compile. The body filter exists so `HALT` never reaches it. A new upstream macro with a comma in the body needs the filter widened; the printer’s compiler error is the signal.
- **`cs150_check.o` at `-O3`.** Asserts are compile-time, so optimization does not drop them. The object is not in `OBJS` and is not linked, so an empty `.o` is fine.
- **CI without cs150.** `make` and `make check-vendor` succeed. `tests/test_vendor.py` runs from the vendored copies alone. No network and no MIPS toolchain for this check.


## Review amendments (Grok Bot, vs the Grok Build draft)
1. **The build compares, not only `make test`.** The draft ran the upstream comparison only in `make check-vendor`.
   The goal says the build compares, so a cheap POSIX `cmp` stamp rule (`$(VENDOR_STAMP)`) now runs as part of every
   `make` when a vendored or upstream file changed: one warning line per differing file, fatal with `VENDOR_CHECK=fail`,
   silent when the originals are absent. The full diff and semantic checks stay in `check-vendor`.
2. **`CS150_SW ?=`** instead of `:=`, so a vvizard clone that is not at `cs150/extern/vvizard` can still point at a
   cs150 tree (`make CS150_SW=...`).
3. **`types.h` stays compiled.** The draft's guard makes the vendored `types.h` body dead code in the check TU; a
   second TU (`types_check.c`) includes only that header, so drift in it fails the build instead of hiding.
4. **No `sizeof(void *) == 8` assert.** `SM_DATA` has no pointers; the assert would only break 32-bit hosts.
5. Implementation detail to keep: the check objects are prerequisites of `$(TARGET)` but are not in `$(OBJS)` and are
   not linked (the link line uses `$(OBJS)`), and they depend on `$(FLAGS_STAMP)` like every other object, so the
   default `-O3` (or `DEBUG=1`) build compiles them with the same flags.
6. Open question for the user: whether `VENDOR_CHECK` should default to `fail` once the shared7 magic/version bug is
   fixed upstream (the known-drift entry then goes stale and `check-vendor` says so). Until then `warn` is right,
   because the cs150 tree is routinely edited (uncommitted changes to these very headers happened during the proj7
   sync).

#!/bin/sh
# vendor-cmp.sh - byte-compare vvizard's vendored cs150 headers with the cs150 originals.
# Run by every `make` (target vendor-cmp, docs/VENDORED-HEADERS-PLAN.md).  POSIX sh + cmp only.
#
#   usage: scripts/vendor-cmp.sh MODE VENDOR_ROOT CS150_SW FILE...
#     MODE      warn (default: loud banner on stderr, exit 0) | fail (banner, exit 1) | off (nothing)
#     CS150_SW  the cs150 software/ directory (default ../../software); FILE paths are relative to
#               its parent, e.g. software/common/mmio_intr_cop0.h
# No cs150 tree (standalone clone): one quiet line, exit 0, in every mode.
mode=$1; vroot=$2; sw=$3; shift 3
[ "$mode" = off ] && exit 0
if [ ! -d "$sw" ]; then
    echo "vendor-cmp: no cs150 tree at $sw; building against the vendored copies in $vroot/ only"
    exit 0
fi
up=$(dirname "$sw")
n=0; bad=0; report=""
nl='
'
for f in "$@"; do
    n=$((n + 1))
    if [ ! -f "$up/$f" ]; then
        report="$report  MISSING  $f   (not found at $up/$f)$nl"; bad=$((bad + 1))
    elif ! cmp -s "$vroot/$f" "$up/$f"; then
        where=$(cmp "$vroot/$f" "$up/$f" 2>&1 | sed -n 's/.*\(line [0-9]*\).*/first difference at \1/p')
        [ -n "$where" ] || where="lengths differ"
        report="$report  DIFFERS  $f   ($where)$nl"; bad=$((bad + 1))
    fi
done
if [ $bad = 0 ]; then
    echo "vendor-cmp: all $n vendored cs150 headers match $sw"
    exit 0
fi
opt=; [ "$sw" = ../../software ] || opt=" CS150_SW=$sw"
prov=$(sed -n 's/^source_commit: //p' "$vroot/PROVENANCE" 2>/dev/null | cut -c1-12)
bar='##############################################################################'
{
    echo "$bar"
    echo "##"
    echo "##   WARNING: VENDORED cs150 HEADERS DIFFER FROM THE cs150 ORIGINALS"
    echo "##   ($bad of $n files; vvizard is built against its copies in $vroot/)"
    echo "##"
    echo "$bar"
    printf '%s' "$report"
    echo "$bar"
    echo "##   vendored from cs150 ${prov:-unknown} (see $vroot/PROVENANCE)"
    echo "##   cs150 originals: $sw"
    echo "##"
    echo "##   See the diff:   make check-vendor$opt"
    echo "##                   (or: diff -u $vroot/<file> $up/<file>)"
    echo "##   Take cs150's:   make vendor-update$opt   then make test, check src/*.h VV_* and"
    echo "##                   $vroot/known-drift, and commit vendor/ with the fixes"
    echo "##   Strict / quiet: make VENDOR_CHECK=fail  |  make VENDOR_CHECK=off"
    echo "$bar"
} >&2
if [ "$mode" = fail ]; then
    echo "vendor-cmp: VENDOR_CHECK=fail: stopping the build" >&2
    exit 1
fi
exit 0

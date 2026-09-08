#!/bin/sh
# Run crxcheck over a corpus in parallel (one log per job, so lines never interleave) and sum the results.
#   scripts/corpus-check.sh <paths-file> <oracle.tsv> [manifest.tsv|""] [extra crxcheck flags...]
# Env: CRX_JOBS (default 8), CRX_LOG (combined log path). Exit 0 only if everything passed.
set -e
paths=$1; oracle=$2; manifest=$3; shift 3
bin=${CRX_BIN:-$(cd "$(dirname "$0")/.." && pwd)/build/crxcheck}
mflag=""; [ -n "$manifest" ] && mflag="-m $manifest"
log=${CRX_LOG:-/tmp/crxcheck.$$.log}
dir=$(mktemp -d)
tr '\n' '\0' < "$paths" | xargs -0 -P "${CRX_JOBS:-8}" -n 200 sh -c '"$0" "$@" > "'"$dir"'/$$.log" 2>&1 || true' "$bin" $mflag -o "$oracle" "$@"
cat "$dir"/*.log > "$log"; rm -rf "$dir"
grep -v -e '^crxcheck' -e '^headers-ok' -e '^exact' "$log" | head -20
awk '/^crxcheck/ { ok += $(NF-15); n += $(NF-12); u += substr($(NF-11), 2); f += $(NF-9); no += $(NF-7) } END { printf "TOTAL: %d ok of %d (%d unsupported, %d FAIL, %d without oracle)\n", ok, n, u, f, no; exit (f == 0 && u == 0 && n > 0) ? 0 : 1 }' "$log"

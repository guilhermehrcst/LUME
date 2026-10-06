#!/bin/bash
# Primary E2-R1 timing matrix for one compiler: the unmodified E2 benchmark driven by the E2 run logic
# (fresh process per glibc_default_fresh cell), then a fail-closed validation of the output.
# usage: run_primary_matrix.sh <lume_e2_bench> <out-dir> <replicates> [--smoke]
# --smoke shrinks the matrix (tooling test only; the result is not scientific data).
set -eu
BENCH=$1; OUT=$2; REPS=$3; MODE=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "refusing to reuse non-empty $OUT" >&2; exit 2; fi
mkdir -p "$OUT"
EXTRA=()
if [ "$MODE" = "--smoke" ]; then
  export E2_DTYPES="f32 i32" E2_KS="1 3" E2_SIZES="64 4096"
  EXTRA=(reps=2 warmup=1)
fi
"$ROOT/experiments/e2-materialize-vs-recompute/run_e2.sh" "$BENCH" "$OUT" "$REPS" "${EXTRA[@]}"
if [ "$MODE" = "--smoke" ]; then
  python3 "$HERE/analyze_cross_machine.py" --validate-bundle-dir "$OUT" --smoke
else
  python3 "$HERE/analyze_cross_machine.py" --validate-bundle-dir "$OUT" --replicates "$REPS"
fi

#!/bin/bash
# Runs the E2 matrix: per replicate, one process for the arena regimes and one
# process per cell for glibc_default_fresh (so glibc's dynamic malloc thresholds
# start from their initial values in every cell).
# usage: run_e2.sh <bench-binary> <results-dir> <replicates> [extra bench args for the arena run...]
set -u
BENCH=$1; OUT=$2; REPS=$3; shift 3
EXTRA=("$@")
DTYPES="f32 i32"
KS="1 2 3 4 6 8"
SIZES="64 1024 4096 16384 65536 262144 1048576 4194304 16777216"
mkdir -p "$OUT"
{
  echo "date_utc=$(date -u +%FT%TZ)"
  echo "commit=$(git rev-parse HEAD)"
  echo "dirty=$(git status --porcelain | wc -l)"
  echo "uname=$(uname -a)"
  echo "bench=$BENCH"
  echo "extra_args=${EXTRA[*]}"
  grep -E "SwapTotal|SwapFree|MemTotal" /proc/meminfo
} > "$OUT/run_meta.txt"
for r in $(seq 1 "$REPS"); do
  # in-process arena regimes
  env -u GLIBC_TUNABLES "$BENCH" out="$OUT/raw_r${r}_arena.csv" plans="$OUT/strategies_r${r}_arena.csv" replicate=$r \
      regimes=arena_warm,arena_cold "${EXTRA[@]}" 2> "$OUT/log_r${r}_arena.txt" || { echo "FAILED r$r arena"; exit 1; }
  # fresh process per cell for the default allocator
  RAW="$OUT/raw_r${r}_fresh.csv"; PLANS="$OUT/strategies_r${r}_fresh.csv"; : > "$RAW"; : > "$PLANS"; first=1
  for dt in $DTYPES; do for k in $KS; do for n in $SIZES; do
    env -u GLIBC_TUNABLES "$BENCH" out="$OUT/.cell.csv" plans="$OUT/.cellp.csv" replicate=$r regimes=glibc_default_fresh \
        dtypes=$dt ks=$k sizes=$n "${EXTRA[@]}" 2>> "$OUT/log_r${r}_fresh.txt" || { echo "FAILED fresh $dt k=$k n=$n"; exit 1; }
    if [ $first -eq 1 ]; then cat "$OUT/.cell.csv" >> "$RAW"; cat "$OUT/.cellp.csv" >> "$PLANS"; first=0
    else tail -n +2 "$OUT/.cell.csv" >> "$RAW"; tail -n +2 "$OUT/.cellp.csv" >> "$PLANS"; fi
  done; done; done
  cp "$OUT/.cell.csv.meta" "$OUT/raw_r${r}_fresh.csv.meta"
  echo "replicate $r done $(date -u +%T)"
done
rm -f "$OUT/.cell.csv" "$OUT/.cellp.csv" "$OUT/.cell.csv.meta"
grep -E "SwapFree|SwapTotal" /proc/meminfo > "$OUT/swap_after.txt"
echo "ALL DONE $(date -u +%FT%TZ)" > "$OUT/DONE"

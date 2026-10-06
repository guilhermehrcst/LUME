#!/bin/bash
# Runs the full E1 matrix: for each replicate, three independent processes
# (default tunables with the in-process regimes, T1, T2).
# usage: run_e1.sh <bench-binary> <results-dir> <replicates> [extra bench args...]
set -u
BENCH=$1; OUT=$2; REPS=$3; shift 3
EXTRA=("$@")
T1="glibc.malloc.trim_threshold=1073741824:glibc.malloc.mmap_threshold=33554432"
T2="glibc.malloc.mmap_threshold=131072"
mkdir -p "$OUT"
{
  echo "date_utc=$(date -u +%FT%TZ)"
  echo "commit=$(git rev-parse HEAD)"
  echo "dirty=$(git status --porcelain | wc -l)"
  echo "uname=$(uname -a)"
  echo "bench=$BENCH"
  echo "extra_args=${EXTRA[*]}"
} > "$OUT/run_meta.txt"
for r in $(seq 1 "$REPS"); do
  env -u GLIBC_TUNABLES "$BENCH" out="$OUT/raw_r${r}_default.csv" plans="$OUT/plans_r${r}_default.csv" replicate=$r \
      regimes=glibc_default,arena_cold,arena_warm "${EXTRA[@]}" 2> "$OUT/log_r${r}_default.txt" || { echo "FAILED r$r default"; exit 1; }
  GLIBC_TUNABLES="$T1" "$BENCH" out="$OUT/raw_r${r}_t1.csv" plans="$OUT/plans_r${r}_t1.csv" replicate=$r \
      regimes=glibc_t1 "${EXTRA[@]}" 2> "$OUT/log_r${r}_t1.txt" || { echo "FAILED r$r t1"; exit 1; }
  GLIBC_TUNABLES="$T2" "$BENCH" out="$OUT/raw_r${r}_t2.csv" plans="$OUT/plans_r${r}_t2.csv" replicate=$r \
      regimes=glibc_t2 "${EXTRA[@]}" 2> "$OUT/log_r${r}_t2.txt" || { echo "FAILED r$r t2"; exit 1; }
  echo "replicate $r done $(date -u +%T)"
done
echo "ALL DONE $(date -u +%FT%TZ)" > "$OUT/DONE"

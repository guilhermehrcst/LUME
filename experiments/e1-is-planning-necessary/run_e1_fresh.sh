#!/bin/bash
# Post-hoc supplementary regime glibc_default_fresh: one process per
# (program, N) cell under default tunables, so glibc's dynamic malloc
# thresholds start from their initial values in every cell.
# usage: run_e1_fresh.sh <bench-binary> <results-dir> <replicates>
set -u
BENCH=$1; OUT=$2; REPS=$3
PROGRAMS="single final_left final_right inter_out after_use chain3 chain4 final_left_i32 chain3_i32"
SIZES="64 1024 4096 16384 65536 262144 1048576 4194304 16777216"
mkdir -p "$OUT"
for r in $(seq 1 "$REPS"); do
  RAW="$OUT/raw_fresh_r${r}.csv"; PLANS="$OUT/plans_fresh_r${r}.csv"; : > "$RAW"; : > "$PLANS"; first=1
  for p in $PROGRAMS; do
    sizes="$SIZES"
    if [ "$p" = final_left ] || [ "$p" = chain3 ]; then sizes="$SIZES 67108864"; fi
    for n in $sizes; do
      env -u GLIBC_TUNABLES "$BENCH" out="$OUT/.cell.csv" plans="$OUT/.cellp.csv" replicate=$r regimes=glibc_default_fresh \
          programs=$p sizes=$n 2>> "$OUT/log_fresh_r${r}.txt" || { echo "FAILED $p $n"; exit 1; }
      if [ $first -eq 1 ]; then cat "$OUT/.cell.csv" >> "$RAW"; cat "$OUT/.cellp.csv" >> "$PLANS"; first=0
      else tail -n +2 "$OUT/.cell.csv" >> "$RAW"; tail -n +2 "$OUT/.cellp.csv" >> "$PLANS"; fi
    done
  done
  cp "$OUT/.cell.csv.meta" "$OUT/raw_fresh_r${r}.csv.meta"
  echo "fresh replicate $r done $(date -u +%T)"
done
rm -f "$OUT/.cell.csv" "$OUT/.cellp.csv" "$OUT/.cell.csv.meta"
echo "ALL DONE $(date -u +%FT%TZ)" > "$OUT/DONE"

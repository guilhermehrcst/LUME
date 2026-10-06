#!/bin/bash
# Secondary PMU characterization (separate dataset, never mixed with primary timing).
# usage: run_pmu_subset.sh <lume_e2r1_pmu> <out-dir> <compiler-label> [--smoke]
set -eu
PMU=$1; OUT=$2; LABEL=$3; MODE=${4:-}
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "refusing to reuse non-empty $OUT" >&2; exit 2; fi
mkdir -p "$OUT"
"$PMU" probe=1 out="$OUT/probe_$LABEL.csv"
# Fixed subset (preregistered): f32,i32 x k=1..4 x N=4Ki,64Ki,1Mi,4Mi, arena_warm, M and R.
ARGS=(dtypes=f32,i32 ks=1,2,3,4 sizes=4096,65536,1048576,4194304 strategies=M,R reps=15 warmup=3)
[ "$MODE" = "--smoke" ] && ARGS=(dtypes=f32 ks=1,3 sizes=4096 strategies=M,R reps=2 warmup=1)
# Three passes of at most four hardware events each, to avoid multiplexing.
GROUPS_=(
  "A:cycles,instructions,branches,branch-misses"
  "B:cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses"
  "C:LLC-loads,LLC-load-misses,dTLB-load-misses,page-faults,minor-faults,major-faults"
)
for g in "${GROUPS_[@]}"; do
  name=${g%%:*}; ev=${g#*:}
  # Skip a pass only if none of its events can be opened (it is then recorded as unsupported by the probe).
  usable=0
  for e in ${ev//,/ }; do
    if grep -q "^$e,1," "$OUT/probe_$LABEL.csv"; then usable=1; fi
  done
  if [ "$usable" -eq 0 ]; then echo "pass $name: no supported event, skipped (see probe_$LABEL.csv)" | tee -a "$OUT/pmu_log_$LABEL.txt"; continue; fi
  "$PMU" out="$OUT/pmu_${LABEL}_$name.csv" compiler="$LABEL" replicate=1 events="$ev" "${ARGS[@]}" 2>> "$OUT/pmu_log_$LABEL.txt"
done
echo "pmu done $LABEL" | tee -a "$OUT/pmu_log_$LABEL.txt"

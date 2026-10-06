#!/bin/bash
# One-command E2-R1 replication on ONE physical host.
#   experiments/e2-r1/run_replication.sh <machine-id> [--no-pmu] [--replicates N]
# Smoke test of the tooling only (never scientific data; allowed on virtual hosts):
#   LUME_R1_ALLOW_VIRTUAL=1 experiments/e2-r1/run_replication.sh SMOKE-<name> --smoke
#
# Fail closed: refuses a dirty tree, a non-physical host, an existing result, failing unit tests,
# non-primary compile flags, any correctness mismatch or invalid cell. Output goes to
# docs/experiments/e2-r1-results/<machine-id>/ only after everything validated; never overwritten.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
ID=${1:-}; shift || true
SMOKE=0; PMU=1; REPLICATES=3
while [ $# -gt 0 ]; do
  case "$1" in
    --smoke) SMOKE=1 ;;
    --no-pmu) PMU=0 ;;
    --replicates) REPLICATES=$2; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
die() { echo "REFUSED: $1" >&2; exit "${2:-2}"; }
[ -n "$ID" ] || die "usage: run_replication.sh <machine-id> [--smoke] [--no-pmu] [--replicates N]"
echo "$ID" | grep -qE '^[A-Za-z0-9][A-Za-z0-9._-]*$' || die "machine-id must match [A-Za-z0-9._-]+"
[ "$SMOKE" -eq 1 ] && REPLICATES=1
RESULTS="$ROOT/docs/experiments/e2-r1-results"
FINAL="$RESULTS/$ID"; TARBALL="$RESULTS/$ID.tar.gz"
[ ! -e "$FINAL" ] || die "$FINAL already exists (results are never overwritten)"
[ ! -e "$TARBALL" ] || die "$TARBALL already exists"

# 1. clean tree (scientific runs only)
DIRTY=$(git -C "$ROOT" status --porcelain | wc -l)
if [ "$SMOKE" -eq 0 ] && [ "$DIRTY" -ne 0 ]; then die "working tree is dirty ($DIRTY entries); commit or stash first"; fi
if [ "$SMOKE" -eq 1 ] && ! echo "$ID" | grep -q '^SMOKE-'; then die "--smoke requires a machine-id starting with SMOKE-"; fi

# 2. physical host (fail closed)
STAGE_ROOT="${R1_STAGING:-$ROOT/build/e2-r1-staging}"; mkdir -p "$STAGE_ROOT"
STAGE=$(mktemp -d "$STAGE_ROOT/$ID.XXXXXX")
"$HERE/check_physical_host.sh" --report > "$STAGE/physical.txt" 2>&1 || true
if ! grep -q '^PHYSICAL=yes' "$STAGE/physical.txt"; then
  if [ "$SMOKE" -eq 1 ] && [ "${LUME_R1_ALLOW_VIRTUAL:-0}" = 1 ]; then
    echo "NOTE: host is not physical; this is a tooling smoke run, physical=false, never R1 data." >&2
  else
    cat "$STAGE/physical.txt" >&2; rm -rf "$STAGE"
    die "this host is virtualized or containerized; R1 primary data must come from a physical host" 3
  fi
fi

# 3. environment, swap baseline
"$HERE/capture_environment.sh" "$ROOT" > "$STAGE/environment.txt"
SWAP_TOTAL=$(sed -n 's/^SwapTotal:[[:space:]]*\([0-9]*\).*/\1/p' /proc/meminfo)
vm() { sed -n "s/^$1 //p" /proc/vmstat; }
IN0=$(vm pswpin); OUT0=$(vm pswpout)
START=$(date -u +%FT%TZ)

# 4. per available compiler: configure, build, unit tests, primary matrix, PMU, codegen
BUILD_ROOT="${R1_BUILD_ROOT:-$ROOT/build/e2-r1}"
NCOMP=0
for pair in "g++:gcc" "clang++:clang"; do
  cxx=${pair%%:*}; name=${pair##*:}
  command -v "$cxx" >/dev/null 2>&1 || { echo "$cxx not installed: skipping $name" | tee -a "$STAGE/skipped_compilers.txt"; continue; }
  B="$BUILD_ROOT/$name"; rm -rf "$B"
  cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$cxx" > "$STAGE/cmake_$name.log" 2>&1 || die "cmake configure failed for $name (see $STAGE/cmake_$name.log)" 4
  if grep -rE 'march=native|ffast-math|Ofast|-Os|-O0|-O1|-O2' "$B"/src/CMakeFiles/lume.dir/flags.make > /dev/null 2>&1; then die "non-primary compile flags detected in $name build" 4; fi
  grep -q -- '-O3 -DNDEBUG' "$B/src/CMakeFiles/lume.dir/flags.make" || die "$name build is not '-O3 -DNDEBUG'" 4
  grep -h CXX_FLAGS "$B/src/CMakeFiles/lume.dir/flags.make" > "$STAGE/flags_$name.txt"
  cmake --build "$B" -j"$(nproc)" > "$STAGE/build_$name.log" 2>&1 || die "build failed for $name" 4
  ctest --test-dir "$B" -j2 --output-on-failure > "$STAGE/ctest_$name.log" 2>&1 || die "unit tests failed for $name (see $STAGE/ctest_$name.log)" 5
  MODE=""; [ "$SMOKE" -eq 1 ] && MODE="--smoke"
  "$HERE/run_primary_matrix.sh" "$B/experiments/e2-materialize-vs-recompute/lume_e2_bench" "$STAGE/$name" "$REPLICATES" $MODE || die "primary matrix failed or did not validate for $name" 6
  if [ "$PMU" -eq 1 ]; then
    "$HERE/run_pmu_subset.sh" "$B/experiments/e2-r1/lume_e2r1_pmu" "$STAGE/pmu_$name" "$name" $MODE || die "PMU subset failed for $name" 7
  fi
  NCOMP=$((NCOMP+1))
done
[ "$NCOMP" -ge 1 ] || die "no supported compiler found" 4
"$HERE/generate_codegen_report.sh" "$ROOT" "$STAGE/codegen"

# 5. swap activity, manifest, checksums, tarball, atomic move
IN1=$(vm pswpin); OUT1=$(vm pswpout)
{ echo "swap_total_kb=$SWAP_TOTAL"; echo "pswpin_delta=$((IN1-IN0))"; echo "pswpout_delta=$((OUT1-OUT0))"; } > "$STAGE/swap_activity.txt"
{
  echo "machine_id=$ID"; echo "smoke=$SMOKE"; echo "replicates=$REPLICATES"
  echo "git_sha=$(git -C "$ROOT" rev-parse HEAD)"; echo "git_dirty_entries_at_start=$DIRTY"
  echo "started_utc=$START"; echo "finished_utc=$(date -u +%FT%TZ)"
  echo "command=run_replication.sh $ID$([ "$SMOKE" -eq 1 ] && echo ' --smoke')$([ "$PMU" -eq 0 ] && echo ' --no-pmu') --replicates $REPLICATES"
  echo "compilers=$(ls -d "$STAGE"/gcc "$STAGE"/clang 2>/dev/null | xargs -n1 basename | tr '\n' ' ')"
} > "$STAGE/MANIFEST.txt"
(cd "$STAGE" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
mkdir -p "$RESULTS"
mv "$STAGE" "$FINAL"
tar -C "$RESULTS" -czf "$TARBALL" "$ID"
(cd "$RESULTS" && sha256sum "$ID.tar.gz" > "$ID.tar.gz.sha256")
echo "DONE: $FINAL"; echo "bundle: $TARBALL ($(cut -d' ' -f1 "$TARBALL.sha256"))"
[ "$SMOKE" -eq 1 ] && echo "SMOKE bundle: tooling test only, delete it; it is excluded from the analysis."
exit 0

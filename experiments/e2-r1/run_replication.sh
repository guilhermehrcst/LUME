#!/bin/bash
# One-command E2-R1 replication on ONE physical host.
#   experiments/e2-r1/run_replication.sh <machine-id> [--no-pmu] [--replicates N]
# Smoke test of the tooling only (never scientific data; allowed on virtual hosts):
#   LUME_R1_ALLOW_VIRTUAL=1 experiments/e2-r1/run_replication.sh SMOKE-<name> --smoke
#
# Fail closed: refuses a dirty tree, less than 8 GiB MemAvailable (check_resources.sh), a non-physical
# host, an existing result, failing unit tests, non-primary compile flags, any correctness mismatch or
# invalid cell, and swap activity during the run. Output goes to
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

# 1b. resources before any expensive work: preregistered >= 8 GiB MemAvailable (swap does not count).
#     Always the live /proc/meminfo; a smoke run below the threshold is allowed and labelled non-primary.
RES_MODE=primary; [ "$SMOKE" -eq 1 ] && RES_MODE=smoke
RES_OUT=$("$HERE/check_resources.sh" --mode "$RES_MODE") || die "resource preflight failed (see the message above)" 8

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

# 3. environment, resource preflight result, swap baseline
"$HERE/capture_environment.sh" "$ROOT" > "$STAGE/environment.txt"
printf '%s\n' "$RES_OUT" > "$STAGE/resources.txt"
SWAP_TOTAL=$(sed -n 's/^SwapTotal:[[:space:]]*\([0-9]*\).*/\1/p' /proc/meminfo)
# Swap evidence: the live /proc/vmstat is copied into the staging directory (it stays there as evidence)
# and checked now, so unreliable counters stop a primary run before any workload (check_swap.sh).
SWAP_MODE=primary; [ "$SMOKE" -eq 1 ] && SWAP_MODE=smoke
cat /proc/vmstat > "$STAGE/vmstat_before.txt" || die "cannot read /proc/vmstat; the preregistered 'no active swap' cannot be verified" 10
"$HERE/check_swap.sh" --mode "$SWAP_MODE" --before "$STAGE/vmstat_before.txt" --after "$STAGE/vmstat_before.txt" > /dev/null \
  || die "swap counters in /proc/vmstat are missing or unreliable; the preregistered 'no active swap' cannot be verified" 10
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
cat /proc/vmstat > "$STAGE/vmstat_after.txt" || die "cannot read /proc/vmstat after the run; 'no active swap' cannot be verified; not published, diagnostics kept in $STAGE" 10
# >>> swap publication gate (executed verbatim by test_check_swap.sh)
# Defense in depth (the analysis also marks swap activity INCONCLUSIVE): a primary run with swap activity
# or with unreliable swap evidence is not published. As with every refusal, the staging directory is kept.
SWAP_RC=0
SWAP_OUT=$("$HERE/check_swap.sh" --mode "$SWAP_MODE" --before "$STAGE/vmstat_before.txt" --after "$STAGE/vmstat_after.txt") || SWAP_RC=$?
{ echo "swap_total_kb=$SWAP_TOTAL"; printf '%s\n' "$SWAP_OUT"; } > "$STAGE/swap_activity.txt"
case "$SWAP_RC" in
  0) ;;
  1) echo "swap activity during the run violates the preregistered 'no active swap'; not published" > "$STAGE/INVALID_SWAP_ACTIVITY.txt"
     die "swap activity during the run (see $STAGE/swap_activity.txt); the preregistration requires no active swap. Not published; diagnostics kept in $STAGE" 9 ;;
  *) echo "swap evidence missing or unreliable; 'no active swap' cannot be verified; not published" > "$STAGE/INVALID_SWAP_EVIDENCE.txt"
     die "swap counters in /proc/vmstat are missing or unreliable (see $STAGE/swap_activity.txt); 'no active swap' cannot be verified. Not published; diagnostics kept in $STAGE" 10 ;;
esac
# <<< swap publication gate
{
  echo "machine_id=$ID"; echo "smoke=$SMOKE"; echo "replicates=$REPLICATES"
  echo "git_sha=$(git -C "$ROOT" rev-parse HEAD)"; echo "git_dirty_entries_at_start=$DIRTY"
  echo "started_utc=$START"; echo "finished_utc=$(date -u +%FT%TZ)"
  echo "command=run_replication.sh $ID$([ "$SMOKE" -eq 1 ] && echo ' --smoke')$([ "$PMU" -eq 0 ] && echo ' --no-pmu') --replicates $REPLICATES"
  echo "compilers=$(ls -d "$STAGE"/gcc "$STAGE"/clang 2>/dev/null | xargs -n1 basename | tr '\n' ' ')"
} > "$STAGE/MANIFEST.txt"
(cd "$STAGE" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
# >>> publication (executed verbatim by test_publication.sh)
# The start-up checks ran hours ago. Re-check for a useful early error, but the final directory
# publication must itself be an atomic no-replace rename: mv -T can overwrite an empty destination.
# The tarball and checksum use shell noclobber redirections, never tar -f <destination>.
mkdir -p "$RESULTS"
[ ! -e "$FINAL" ] || die "$FINAL appeared during the run; not published, diagnostics kept in $STAGE" 11
[ ! -e "$TARBALL" ] || die "$TARBALL appeared during the run; not published, diagnostics kept in $STAGE" 11
[ ! -e "$TARBALL.sha256" ] || die "$TARBALL.sha256 appeared during the run; not published, diagnostics kept in $STAGE" 11
python3 "$HERE/rename_noreplace.py" "$STAGE" "$FINAL" \
  || die "could not atomically publish $STAGE as $FINAL without replacement; diagnostics kept in $STAGE" 11
set -C
tar -C "$RESULTS" -czf - "$ID" > "$TARBALL" || die "could not create $TARBALL without overwriting" 11
(cd "$RESULTS" && sha256sum "$ID.tar.gz" > "$ID.tar.gz.sha256") || die "could not create $TARBALL.sha256 without overwriting" 11
set +C
# <<< publication
echo "DONE: $FINAL"; echo "bundle: $TARBALL ($(cut -d' ' -f1 "$TARBALL.sha256"))"
[ "$SMOKE" -eq 1 ] && echo "SMOKE bundle: tooling test only, delete it; it is excluded from the analysis."
exit 0

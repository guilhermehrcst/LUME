#!/bin/bash
# Deterministic regression tests for check_resources.sh (E2-R1 resource preflight).
# Uses meminfo fixtures only; the RAM of the machine running the test is never consulted.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CHECK="$HERE/check_resources.sh"
RUNNER="$HERE/run_replication.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0

# A realistic /proc/meminfo with the MemAvailable line replaced by $1 (or removed when $1 is empty).
fixture() {
  local f="$TMP/meminfo.$RANDOM$RANDOM"
  {
    echo "MemTotal:       16480952 kB"
    echo "MemFree:          911232 kB"
    [ -n "$1" ] && echo "$1"
    echo "Buffers:           12044 kB"
    echo "SwapTotal:      33554432 kB"
    echo "SwapFree:       33554432 kB"
  } > "$f"
  echo "$f"
}

# expect <name> <mode> <meminfo-file> <exit> <RESOURCES=...> [<stderr substring>...]
expect() {
  local name=$1 mode=$2 file=$3 want_rc=$4 want_res=$5; shift 5
  local out err rc
  out=$(bash "$CHECK" --mode "$mode" --meminfo "$file" 2> "$TMP/err"); rc=$?
  err=$(cat "$TMP/err")
  local ok=1
  [ "$rc" -eq "$want_rc" ] || ok=0
  printf '%s\n' "$out" | grep -qx "$want_res" || ok=0
  for s in "$@"; do printf '%s\n' "$err" | grep -qF -- "$s" || ok=0; done
  if [ "$ok" -eq 1 ]; then echo "ok   $name"; else echo "FAIL $name (rc=$rc, out=[$out], err=[$err])"; fails=$((fails+1)); fi
}

# Boundaries: 8 GiB = 8 * 1024 * 1024 = 8388608 "kB" (the kernel's kB are 1024-byte units).
expect "primary 8 GiB - 1 KiB refused" primary "$(fixture 'MemAvailable:    8388607 kB')" 1 RESOURCES=refused \
  "8388607 kB" "8388608 kB" "swap does not satisfy it" "preregistered"
expect "primary exactly 8 GiB passes" primary "$(fixture 'MemAvailable:    8388608 kB')" 0 RESOURCES=ok
expect "primary 8 GiB + 1 KiB passes" primary "$(fixture 'MemAvailable:    8388609 kB')" 0 RESOURCES=ok
expect "primary 15.2 GiB passes" primary "$(fixture 'MemAvailable:   15969088 kB')" 0 RESOURCES=ok
# 8 GB (decimal) is not 8 GiB: 8,000,000 kB must be refused.
expect "primary 8,000,000 kB (decimal 8 GB-ish) refused" primary "$(fixture 'MemAvailable:    8000000 kB')" 1 RESOURCES=refused
# A host with about 7.2 GiB available (like the FX-6300 tooling host) is refused, whatever its swap.
expect "primary 7.2 GiB refused, 32 GiB swap ignored" primary "$(fixture 'MemAvailable:    7549747 kB')" 1 RESOURCES=refused "7.20 GiB"
expect "leading zeros are decimal, not octal" primary "$(fixture 'MemAvailable:  008388608 kB')" 0 RESOURCES=ok
# 010000000 is 10,000,000 decimal (pass); read as octal it would be 2,097,152 (wrongly refused).
expect "leading zero with octal-valid digits is decimal" primary "$(fixture 'MemAvailable:  010000000 kB')" 0 RESOURCES=ok

# Missing or malformed evidence fails closed in both modes.
for mode in primary smoke; do
  expect "$mode: MemAvailable missing" "$mode" "$(fixture '')" 2 RESOURCES=unknown "cannot be verified"
  expect "$mode: non-numeric value" "$mode" "$(fixture 'MemAvailable:   abc kB')" 2 RESOURCES=unknown
  expect "$mode: negative value" "$mode" "$(fixture 'MemAvailable:   -9000000 kB')" 2 RESOURCES=unknown
  expect "$mode: wrong unit (MB)" "$mode" "$(fixture 'MemAvailable:   9000000 MB')" 2 RESOURCES=unknown
  expect "$mode: no unit" "$mode" "$(fixture 'MemAvailable:   9000000')" 2 RESOURCES=unknown
  expect "$mode: absurdly long number" "$mode" "$(fixture 'MemAvailable:   1234567890123456789012 kB')" 2 RESOURCES=unknown
  dup=$(fixture 'MemAvailable:    9000000 kB'); echo 'MemAvailable:    1000 kB' >> "$dup"
  expect "$mode: duplicated MemAvailable" "$mode" "$dup" 2 RESOURCES=unknown
  expect "$mode: unreadable file" "$mode" "$TMP/does-not-exist" 2 RESOURCES=unknown "cannot read"
done

# Smoke below the threshold is allowed but labelled non-primary; at or above it is simply ok.
expect "smoke 7.2 GiB allowed, labelled non-primary" smoke "$(fixture 'MemAvailable:    7549747 kB')" 0 RESOURCES=non-primary \
  "NON-PRIMARY TOOLING VALIDATION" "not R1 primary data" "excluded from scientific analysis"
expect "smoke 8 GiB - 1 KiB allowed, labelled non-primary" smoke "$(fixture 'MemAvailable:    8388607 kB')" 0 RESOURCES=non-primary
expect "smoke 8 GiB ok" smoke "$(fixture 'MemAvailable:    8388608 kB')" 0 RESOURCES=ok

# Reported quantities and usage errors.
out=$(bash "$CHECK" --mode primary --meminfo "$(fixture 'MemAvailable:   15969088 kB')")
if printf '%s\n' "$out" | grep -qx REQUIRED_KIB=8388608 && printf '%s\n' "$out" | grep -qx MEMAVAILABLE_KIB=15969088; then
  echo "ok   reports REQUIRED_KIB=8388608 and the observed value"; else echo "FAIL reported quantities: [$out]"; fails=$((fails+1)); fi
if bash "$CHECK" --meminfo "$(fixture 'MemAvailable:   15969088 kB')" > /dev/null 2>&1; then
  echo "FAIL missing --mode accepted"; fails=$((fails+1)); else echo "ok   missing --mode is a usage error"; fi

# Integration guards on the runner (static): the preflight runs before staging and any build, reads the
# live /proc/meminfo (no --meminfo passed, no override variable), and its failure stops the run.
call=$(grep -n 'check_resources.sh' "$RUNNER" | grep -v '^[0-9]*:#' | head -1 | cut -d: -f1)
stage=$(grep -n 'mktemp -d' "$RUNNER" | head -1 | cut -d: -f1)
cmake=$(grep -n 'cmake -S' "$RUNNER" | head -1 | cut -d: -f1)
if [ -n "$call" ] && [ -n "$stage" ] && [ -n "$cmake" ] && [ "$call" -lt "$stage" ] && [ "$call" -lt "$cmake" ]; then
  echo "ok   runner calls the preflight (line $call) before staging (line $stage) and cmake (line $cmake)"; else
  echo "FAIL runner ordering: call=$call stage=$stage cmake=$cmake"; fails=$((fails+1)); fi
if grep -q -- '--meminfo' "$RUNNER"; then echo "FAIL runner passes --meminfo (test seam reachable from a scientific run)"; fails=$((fails+1)); else
  echo "ok   runner never passes --meminfo"; fi
if sed -n "${call}p" "$RUNNER" | grep -q '|| die'; then echo "ok   preflight failure stops the runner"; else
  echo "FAIL preflight result is not enforced by the runner"; fails=$((fails+1)); fi

if [ "$fails" -eq 0 ]; then echo "resource preflight regression tests passed"; exit 0; fi
echo "resource preflight regression tests FAILED ($fails)"; exit 1

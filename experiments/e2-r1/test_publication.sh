#!/bin/bash
# Regression test for the R1 publication step: the marked block of run_replication.sh is extracted and
# executed verbatim. A result that appeared while the run was in progress (same machine-id) must never be
# nested into, overwritten or re-tarred; the refused run keeps its staging directory.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
RUNNER="$HERE/run_replication.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; fails=$((fails+1)); }

block="$TMP/publish.sh"
{ echo 'set -eu'; grep -m1 '^die() ' "$RUNNER"; sed -n '/^# >>> publication/,/^# <<< publication/p' "$RUNNER"; echo 'echo PUBLISHED'; } > "$block"
grep -q 'mv -T' "$block" && pass "publication block extracted from the runner" || fail "publication block not found"

# setup <dir>: a staging directory holding a small bundle, results root, ids
setup() {
  local d=$1; mkdir -p "$d/stage" "$d/results"; echo second-run > "$d/stage/MANIFEST.txt"
  (cd "$d/stage" && sha256sum MANIFEST.txt > SHA256SUMS)
}
run() { # <dir> -> sets rc and out
  out=$(cd "$1" && ID=x86-test-01 RESULTS="$1/results" FINAL="$1/results/x86-test-01" TARBALL="$1/results/x86-test-01.tar.gz" STAGE="$1/stage" \
        bash -c '. "$0"' "$block" 2>&1); rc=$?
}

d="$TMP/normal"; setup "$d"; run "$d"
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qx PUBLISHED && [ -f "$d/results/x86-test-01/MANIFEST.txt" ] && [ ! -e "$d/stage" ] \
   && (cd "$d/results" && sha256sum -c --quiet x86-test-01.tar.gz.sha256); then pass "normal publication: bundle, tarball and checksum"; else fail "normal publication (rc=$rc out=[$out])"; fi

d="$TMP/final_exists"; setup "$d"; mkdir -p "$d/results/x86-test-01"; echo first-run > "$d/results/x86-test-01/MANIFEST.txt"
run "$d"
if [ "$rc" -ne 0 ] && ! printf '%s\n' "$out" | grep -qx PUBLISHED && [ "$(cat "$d/results/x86-test-01/MANIFEST.txt")" = first-run ] \
   && [ "$(ls "$d/results/x86-test-01")" = MANIFEST.txt ] && [ -f "$d/stage/MANIFEST.txt" ] && [ ! -e "$d/results/x86-test-01.tar.gz" ]; then
  pass "result appeared during the run: refused, first result untouched, nothing nested, staging kept"; else fail "existing result (rc=$rc out=[$out]; $(find "$d/results" | tr '\n' ' '))"; fi

d="$TMP/tar_exists"; setup "$d"; echo "someone else's tarball" > "$d/results/x86-test-01.tar.gz"; before=$(sha256sum < "$d/results/x86-test-01.tar.gz")
run "$d"
if [ "$rc" -ne 0 ] && [ "$(sha256sum < "$d/results/x86-test-01.tar.gz")" = "$before" ] && [ -f "$d/stage/MANIFEST.txt" ] && [ ! -e "$d/results/x86-test-01" ]; then
  pass "tarball appeared during the run: refused, existing tarball untouched, staging kept"; else fail "existing tarball (rc=$rc out=[$out])"; fi

d="$TMP/sha_exists"; setup "$d"; echo "stale" > "$d/results/x86-test-01.tar.gz.sha256"
run "$d"
if [ "$rc" -ne 0 ] && [ "$(cat "$d/results/x86-test-01.tar.gz.sha256")" = stale ] && [ -f "$d/stage/MANIFEST.txt" ] && [ ! -e "$d/results/x86-test-01" ]; then
  pass "checksum file appeared during the run: refused, never overwritten, staging kept"; else fail "existing checksum file (rc=$rc out=[$out])"; fi

# Each barrier on its own: without the re-checks (a result appearing in the instant between check and
# publication), mv -T alone must refuse to nest into a result and noclobber alone must refuse to overwrite.
grep -v '^\[ ! -e "\$\(FINAL\|TARBALL\)' "$block" > "$TMP/no_recheck.sh"; block="$TMP/no_recheck.sh"
if grep -q '^\[ ! -e' "$block"; then fail "re-checks not removed for barrier isolation"; else pass "barrier isolation: re-checks removed"; fi
d="$TMP/iso_final"; setup "$d"; mkdir -p "$d/results/x86-test-01"; echo first-run > "$d/results/x86-test-01/MANIFEST.txt"; run "$d"
if [ "$rc" -ne 0 ] && [ "$(ls "$d/results/x86-test-01")" = MANIFEST.txt ] && [ "$(cat "$d/results/x86-test-01/MANIFEST.txt")" = first-run ] && [ -f "$d/stage/MANIFEST.txt" ]; then
  pass "mv -T alone: never nests into an existing result"; else fail "mv -T alone (rc=$rc; $(find "$d/results" | tr '\n' ' '))"; fi
d="$TMP/iso_tar"; setup "$d"; echo "someone else's tarball" > "$d/results/x86-test-01.tar.gz"; before=$(sha256sum < "$d/results/x86-test-01.tar.gz"); run "$d"
if [ "$rc" -ne 0 ] && [ "$(sha256sum < "$d/results/x86-test-01.tar.gz")" = "$before" ]; then pass "noclobber alone: never overwrites an existing tarball"; else fail "noclobber alone (rc=$rc)"; fi
d="$TMP/iso_sha"; setup "$d"; echo stale > "$d/results/x86-test-01.tar.gz.sha256"; run "$d"
if [ "$rc" -ne 0 ] && [ "$(cat "$d/results/x86-test-01.tar.gz.sha256")" = stale ]; then pass "noclobber alone: never overwrites an existing checksum file"; else fail "noclobber alone, checksum (rc=$rc)"; fi

if [ "$fails" -eq 0 ]; then echo "publication regression tests passed"; exit 0; fi
echo "publication regression tests FAILED ($fails)"; exit 1

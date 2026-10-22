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
grep -Fq 'python3 "$HERE/rename_noreplace.py" "$STAGE" "$FINAL"' "$block" && pass "atomic publication block extracted from the runner" || fail "atomic publication block not found"

# setup <dir>: a staging directory holding a small bundle, results root, ids
setup() {
  local d=$1; mkdir -p "$d/stage" "$d/results"; echo second-run > "$d/stage/MANIFEST.txt"
  (cd "$d/stage" && sha256sum MANIFEST.txt > SHA256SUMS)
}
run() { # <dir> -> sets rc and out
  out=$(cd "$1" && HERE="$HERE" ID=x86-test-01 RESULTS="$1/results" FINAL="$1/results/x86-test-01" TARBALL="$1/results/x86-test-01.tar.gz" STAGE="$1/stage" \
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
# publication), the no-replace rename must reject even an EMPTY destination directory, and shell
# noclobber redirections must reject existing tarball and checksum files.
grep -v '^\[ ! -e "\$\(FINAL\|TARBALL\)' "$block" > "$TMP/no_recheck.sh"; block="$TMP/no_recheck.sh"
if grep -q '^\[ ! -e' "$block"; then fail "re-checks not removed for barrier isolation"; else pass "barrier isolation: re-checks removed"; fi
d="$TMP/iso_final"; setup "$d"; mkdir -p "$d/results/x86-test-01"; echo first-run > "$d/results/x86-test-01/MANIFEST.txt"; run "$d"
if [ "$rc" -ne 0 ] && [ "$(ls "$d/results/x86-test-01")" = MANIFEST.txt ] && [ "$(cat "$d/results/x86-test-01/MANIFEST.txt")" = first-run ] && [ -f "$d/stage/MANIFEST.txt" ]; then
  pass "atomic no-replace rename: never nests into an existing non-empty result"; else fail "atomic rename with existing result (rc=$rc; $(find "$d/results" | tr '\n' ' '))"; fi
# This is the regression the old mv -T implementation missed: an EMPTY final directory appears
# between the last existence check and the move. The final rename must still fail atomically.
d="$TMP/iso_empty"; setup "$d"; mkdir -p "$d/results/x86-test-01"; run "$d"
if [ "$rc" -ne 0 ] && [ -z "$(ls -A "$d/results/x86-test-01")" ] && [ -f "$d/stage/MANIFEST.txt" ] \
   && [ ! -e "$d/results/x86-test-01.tar.gz" ]; then
  pass "atomic no-replace: empty destination directory untouched, staging preserved"
else
  fail "empty directory race (rc=$rc out=[$out])"
fi

# A dangling symlink is still an occupied destination name. '-e' would miss it.
d="$TMP/iso_symlink"; setup "$d"; ln -s absent-directory "$d/results/x86-test-01"; run "$d"
if [ "$rc" -ne 0 ] && [ -L "$d/results/x86-test-01" ] \
   && [ "$(readlink "$d/results/x86-test-01")" = absent-directory ] && [ -f "$d/stage/MANIFEST.txt" ]; then
  pass "atomic no-replace: dangling destination symlink untouched"
else
  fail "dangling symlink destination (rc=$rc out=[$out])"
fi

# Two independent publishers racing for one name must have exactly one winner.
d="$TMP/iso_concurrent"; mkdir -p "$d/results" "$d/first" "$d/second"
echo first > "$d/first/winner"; echo second > "$d/second/winner"
python3 "$HERE/rename_noreplace.py" "$d/first" "$d/results/x86-test-01" > "$d/first.log" 2>&1 &
pid1=$!
python3 "$HERE/rename_noreplace.py" "$d/second" "$d/results/x86-test-01" > "$d/second.log" 2>&1 &
pid2=$!
wait "$pid1"; rc1=$?
wait "$pid2"; rc2=$?
winner=$(cat "$d/results/x86-test-01/winner" 2>/dev/null || true)
if [ "$((rc1 + rc2))" -eq 1 ] && { [ "$winner" = first ] || [ "$winner" = second ]; } \
   && { [ -d "$d/first" ] || [ -d "$d/second" ]; }; then
  pass "concurrent atomic publication: exactly one winner and losing stage preserved"
else
  fail "concurrent atomic publication (rc1=$rc1 rc2=$rc2 winner=$winner)"
fi

d="$TMP/iso_tar"; setup "$d"; echo "someone else's tarball" > "$d/results/x86-test-01.tar.gz"; before=$(sha256sum < "$d/results/x86-test-01.tar.gz"); run "$d"
if [ "$rc" -ne 0 ] && [ "$(sha256sum < "$d/results/x86-test-01.tar.gz")" = "$before" ]; then pass "noclobber alone: never overwrites an existing tarball"; else fail "noclobber alone (rc=$rc)"; fi
d="$TMP/iso_sha"; setup "$d"; echo stale > "$d/results/x86-test-01.tar.gz.sha256"; run "$d"
if [ "$rc" -ne 0 ] && [ "$(cat "$d/results/x86-test-01.tar.gz.sha256")" = stale ]; then pass "noclobber alone: never overwrites an existing checksum file"; else fail "noclobber alone, checksum (rc=$rc)"; fi

if [ "$fails" -eq 0 ]; then echo "publication regression tests passed"; exit 0; fi
echo "publication regression tests FAILED ($fails)"; exit 1

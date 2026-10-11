#!/bin/bash
# Deterministic regression tests for the E2-R1 swap evidence check (check_swap.sh) and for the
# production publication gate in run_replication.sh, which is extracted and executed verbatim.
# Uses vmstat fixtures only; the swap counters of the machine running the test are never consulted.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CHECK="$HERE/check_swap.sh"
RUNNER="$HERE/run_replication.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; fails=$((fails+1)); }

# vmstat fixture: lines given as arguments, surrounded by realistic unrelated counters.
vmstat() {
  local f="$TMP/vm.$RANDOM$RANDOM"
  { echo "nr_free_pages 912345"; for l in "$@"; do printf '%s\n' "$l"; done; echo "pswpin_extra_field 99"; echo "pgfault 123456"; } > "$f"
  echo "$f"
}
OKB=$(vmstat "pswpin 100" "pswpout 200")

# expect <name> <mode> <before> <after> <exit> <swap_activity> [<stdout line>|<stderr substring>...]
expect() {
  local name=$1 mode=$2 before=$3 after=$4 want_rc=$5 want_act=$6; shift 6
  local out rc ok=1
  out=$(bash "$CHECK" --mode "$mode" --before "$before" --after "$after" 2> "$TMP/err"); rc=$?
  [ "$rc" -eq "$want_rc" ] || ok=0
  printf '%s\n' "$out" | grep -qx "swap_activity=$want_act" || ok=0
  for s in "$@"; do
    case "$s" in
      *=*) printf '%s\n' "$out" | grep -qx -- "$s" || ok=0 ;;
      *) grep -qF -- "$s" "$TMP/err" || ok=0 ;;
    esac
  done
  if [ "$ok" -eq 1 ]; then pass "$name"; else fail "$name (rc=$rc out=[$(echo "$out" | tr '\n' ' ')] err=[$(tr '\n' ' ' < "$TMP/err")])"; fi
}

# --- valid evidence
expect "both deltas zero -> PASS" primary "$OKB" "$(vmstat 'pswpin 100' 'pswpout 200')" 0 zero pswpin_delta=0 pswpout_delta=0 swap_evidence=valid
expect "positive pswpin delta -> REFUSE" primary "$OKB" "$(vmstat 'pswpin 101' 'pswpout 200')" 1 positive pswpin_delta=1 pswpout_delta=0 "REFUSED: swap activity"
expect "positive pswpout delta -> REFUSE" primary "$OKB" "$(vmstat 'pswpin 100' 'pswpout 203')" 1 positive pswpin_delta=0 pswpout_delta=3
expect "both deltas positive -> REFUSE" primary "$OKB" "$(vmstat 'pswpin 105' 'pswpout 207')" 1 positive pswpin_delta=5 pswpout_delta=7
expect "leading zeros are decimal (010 -> 012 is +2)" primary "$(vmstat 'pswpin 010' 'pswpout 0200')" "$(vmstat 'pswpin 012' 'pswpout 0200')" 1 positive pswpin_before=10 pswpin_after=12 pswpin_delta=2
expect "leading zeros with 8/9 digits are decimal (09 == 9)" primary "$(vmstat 'pswpin 09' 'pswpout 0')" "$(vmstat 'pswpin 9' 'pswpout 00')" 0 zero pswpin_delta=0
expect "maximum safe value (18 digits), unchanged" primary "$(vmstat 'pswpin 999999999999999999' 'pswpout 999999999999999999')" "$(vmstat 'pswpin 999999999999999999' 'pswpout 999999999999999999')" 0 zero pswpin_delta=0
expect "0 -> maximum safe value is a correct positive delta" primary "$(vmstat 'pswpin 0' 'pswpout 0')" "$(vmstat 'pswpin 999999999999999999' 'pswpout 0')" 1 positive pswpin_delta=999999999999999999

# --- invalid evidence, in the initial snapshot and in the final snapshot
for where in before after; do
  bad() { if [ "$where" = before ]; then echo "$1 $OKB"; else echo "$OKB $1"; fi; }
  case_() { # name <bad vmstat file>
    local b a; read -r b a <<< "$(bad "$2")"
    expect "$where: $1 -> REFUSE" primary "$b" "$a" 2 unknown pswpin_delta=invalid pswpout_delta=invalid swap_evidence=invalid "cannot be verified"
    expect "$where: $1 -> smoke allowed, NOT reported as zero" smoke "$b" "$a" 0 unknown pswpin_delta=invalid "NO verified zero-swap measurement"
  }
  case_ "missing pswpin" "$(vmstat 'pswpout 200')"
  case_ "missing pswpout" "$(vmstat 'pswpin 100')"
  case_ "both counters missing" "$(vmstat)"
  case_ "empty pswpin" "$(vmstat 'pswpin ' 'pswpout 200')"
  case_ "bare pswpin field" "$(vmstat 'pswpin' 'pswpout 200')"
  case_ "non-numeric pswpin" "$(vmstat 'pswpin abc' 'pswpout 200')"
  case_ "arithmetic-expression pswpout" "$(vmstat 'pswpin 100' 'pswpout a[0]+1')"
  case_ "negative pswpout" "$(vmstat 'pswpin 100' 'pswpout -5')"
  case_ "duplicate pswpin" "$(vmstat 'pswpin 100' 'pswpin 100' 'pswpout 200')"
  case_ "19-digit value (exceeds safe maximum)" "$(vmstat 'pswpin 1000000000000000000' 'pswpout 200')"
  case_ "2^63 (signed overflow)" "$(vmstat 'pswpin 9223372036854775808' 'pswpout 200')"
  case_ "2^64 (would wrap to 0)" "$(vmstat 'pswpin 18446744073709551616' 'pswpout 200')"
  case_ "unreadable file" "$TMP/does-not-exist-$where"
done
# The refusal reason must name the actual defect (so each validation branch is observable on its own).
expect "reason: missing counter" primary "$(vmstat 'pswpout 200')" "$OKB" 2 unknown "pswpin appears 0 times"
expect "reason: duplicate counter" primary "$(vmstat 'pswpin 100' 'pswpin 100' 'pswpout 200')" "$OKB" 2 unknown "pswpin appears 2 times"
expect "reason: negative counter" primary "$(vmstat 'pswpin -5' 'pswpout 200')" "$OKB" 2 unknown "pswpin in vm."
expect "reason: negative counter is a value defect" primary "$(vmstat 'pswpin -5' 'pswpout 200')" "$OKB" 2 unknown "is empty, non-numeric, negative or longer than 18 digits"
expect "reason: oversized counter is a value defect" primary "$OKB" "$(vmstat 'pswpin 18446744073709551616' 'pswpout 200')" 2 unknown "is empty, non-numeric, negative or longer than 18 digits"
if bash "$CHECK" --mode primary --before "$(vmstat 'pswpin 100' 'pswpin 100' 'pswpout 200')" --after "$OKB" 2>&1 | grep -q "syntax error\|value too great\|unbound variable"; then
  fail "duplicate counter reaches shell arithmetic"; else pass "duplicate counter never reaches shell arithmetic"; fi
if bash "$CHECK" --mode primary --before "$(vmstat 'pswpin -5' 'pswpout 200')" --after "$OKB" 2>&1 | grep -q "syntax error\|value too great\|unbound variable"; then
  fail "negative counter reaches shell arithmetic"; else pass "negative counter never reaches shell arithmetic"; fi
expect "final pswpin below initial -> REFUSE" primary "$(vmstat 'pswpin 9' 'pswpout 200')" "$(vmstat 'pswpin 5' 'pswpout 200')" 2 unknown "decreased"
expect "final pswpout below initial -> REFUSE" primary "$OKB" "$(vmstat 'pswpin 100' 'pswpout 199')" 2 unknown "decreased"
expect "smoke positive delta -> warning, recorded as positive" smoke "$OKB" "$(vmstat 'pswpin 101' 'pswpout 200')" 0 positive pswpin_delta=1 "WARNING: swap activity"
if bash "$CHECK" --before "$OKB" --after "$OKB" > /dev/null 2>&1; then fail "missing --mode accepted"; else pass "missing --mode is a usage error"; fi
if bash "$CHECK" --mode primary --before "$OKB" > /dev/null 2>&1; then fail "missing --after accepted"; else pass "missing --after is a usage error"; fi

# --- the production publication gate, extracted verbatim from run_replication.sh and executed
gate="$TMP/gate.sh"
{
  echo 'set -eu'
  grep -m1 '^die() ' "$RUNNER"
  grep -m1 '^SWAP_MODE=' "$RUNNER"
  sed -n '/^# >>> swap publication gate/,/^# <<< swap publication gate/p' "$RUNNER"
  echo 'echo PUBLICATION_REACHED'
} > "$gate"
if grep -q 'check_swap.sh' "$gate" && grep -q '^case "\$SWAP_RC"' "$gate"; then pass "gate block extracted from the runner"; else fail "gate block not found in the runner"; fi
# run_gate <name> <SMOKE> <before> <after> <want exit> <want published 0/1> <marker file or -> <swap_activity value>
run_gate() {
  local name=$1 smoke=$2 b=$3 a=$4 want_rc=$5 want_pub=$6 marker=$7 act=$8
  local st="$TMP/stage.$RANDOM$RANDOM" rc out ok=1
  mkdir -p "$st"; cp "$b" "$st/vmstat_before.txt" 2>/dev/null || true; cp "$a" "$st/vmstat_after.txt" 2>/dev/null || true
  out=$(HERE="$HERE" STAGE="$st" SMOKE="$smoke" SWAP_TOTAL=0 bash -c '. "$0"' "$gate" 2>&1); rc=$?
  [ "$rc" -eq "$want_rc" ] || ok=0
  if printf '%s\n' "$out" | grep -qx PUBLICATION_REACHED; then [ "$want_pub" -eq 1 ] || ok=0; else [ "$want_pub" -eq 0 ] || ok=0; fi
  [ "$marker" = - ] || [ -f "$st/$marker" ] || ok=0
  grep -qx "swap_activity=$act" "$st/swap_activity.txt" 2>/dev/null || ok=0
  if [ "$ok" -eq 1 ]; then pass "gate: $name"; else fail "gate: $name (rc=$rc out=[$(echo "$out" | tr '\n' ' ')])"; fi
}
run_gate "primary zero swap -> continues to publication" 0 "$OKB" "$(vmstat 'pswpin 100' 'pswpout 200')" 0 1 - zero
run_gate "primary positive swap -> refused (exit 9), not published" 0 "$OKB" "$(vmstat 'pswpin 100' 'pswpout 201')" 9 0 INVALID_SWAP_ACTIVITY.txt positive
run_gate "primary missing counter -> refused (exit 10), not published" 0 "$OKB" "$(vmstat 'pswpout 200')" 10 0 INVALID_SWAP_EVIDENCE.txt unknown
run_gate "primary corrupted initial snapshot -> refused (exit 10)" 0 "$(vmstat 'pswpin x' 'pswpout 200')" "$OKB" 10 0 INVALID_SWAP_EVIDENCE.txt unknown
run_gate "primary overflowing counter -> refused (exit 10)" 0 "$OKB" "$(vmstat 'pswpin 18446744073709551716' 'pswpout 200')" 10 0 INVALID_SWAP_EVIDENCE.txt unknown
run_gate "primary decreasing counter -> refused (exit 10)" 0 "$OKB" "$(vmstat 'pswpin 99' 'pswpout 200')" 10 0 INVALID_SWAP_EVIDENCE.txt unknown
run_gate "primary unreadable final snapshot -> refused (exit 10)" 0 "$OKB" "$TMP/nonexistent" 10 0 INVALID_SWAP_EVIDENCE.txt unknown
run_gate "smoke positive swap -> published smoke, recorded positive" 1 "$OKB" "$(vmstat 'pswpin 100' 'pswpout 201')" 0 1 - positive
run_gate "smoke missing counter -> published smoke, recorded unknown (not zero)" 1 "$OKB" "$(vmstat 'pswpout 200')" 0 1 - unknown

# --- static guards on the runner
if grep -nE '\$\(\(.*(IN[01]|OUT[01]|pswp)' "$RUNNER" > /dev/null; then fail "runner still does unchecked swap arithmetic"; else pass "runner has no unchecked swap arithmetic"; fi
if [ "$(grep -c 'cat /proc/vmstat > "\$STAGE/vmstat_' "$RUNNER")" -eq 2 ]; then pass "both snapshots are copied from the live /proc/vmstat"; else fail "snapshot sources are not exactly the live /proc/vmstat"; fi
if grep 'check_swap.sh' "$RUNNER" | grep -v '^#' | grep -vq -- '--before "\$STAGE/vmstat_before.txt"'; then fail "runner calls check_swap.sh with another source"; else pass "check_swap.sh only ever receives the staged snapshots"; fi
gate_line=$(grep -n '^# <<< swap publication gate' "$RUNNER" | cut -d: -f1)
mv_line=$(grep -n '^mv "\$STAGE" "\$FINAL"' "$RUNNER" | cut -d: -f1)
pre_line=$(grep -n 'check_swap.sh" --mode "\$SWAP_MODE" --before "\$STAGE/vmstat_before.txt" --after "\$STAGE/vmstat_before.txt"' "$RUNNER" | cut -d: -f1)
cmake_line=$(grep -n 'cmake -S' "$RUNNER" | head -1 | cut -d: -f1)
if [ -n "$gate_line" ] && [ -n "$mv_line" ] && [ "$gate_line" -lt "$mv_line" ]; then pass "gate (line $gate_line) precedes publication (line $mv_line)"; else fail "gate does not precede publication"; fi
if [ -n "$pre_line" ] && [ -n "$cmake_line" ] && [ "$pre_line" -lt "$cmake_line" ]; then pass "initial evidence is checked (line $pre_line) before any build (line $cmake_line)"; else fail "initial evidence not checked before the build"; fi

# --- the analysis contract: an invalid record is never read as zero swap by analyze_cross_machine.py
if command -v python3 > /dev/null 2>&1; then
  st="$TMP/stage.analysis"; mkdir -p "$st"; cp "$OKB" "$st/vmstat_before.txt"
  cp "$(vmstat 'pswpout 200')" "$st/vmstat_after.txt"
  HERE="$HERE" STAGE="$st" SMOKE=1 SWAP_TOTAL=0 bash -c '. "$0"' "$gate" > /dev/null 2>&1
  verdict=$(python3 -I -c '
import sys; sys.path.insert(0, sys.argv[1]); import analyze_cross_machine as a
d = a.read_kv(sys.argv[2])
print("flagged" if d.get("pswpout_delta", "0") not in ("0", "") or d.get("pswpin_delta", "0") not in ("0", "") else "zero")' "$HERE" "$st/swap_activity.txt")
  if [ "$verdict" = flagged ]; then pass "analysis treats an invalid swap record as swap activity, not zero"; else fail "analysis reads an invalid swap record as zero"; fi
fi

if [ "$fails" -eq 0 ]; then echo "swap evidence regression tests passed"; exit 0; fi
echo "swap evidence regression tests FAILED ($fails)"; exit 1

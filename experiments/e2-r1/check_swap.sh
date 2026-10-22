#!/bin/bash
# E2-R1 swap-activity evidence check: enforces the preregistered "no active swap during the run"
# (docs/experiments/e2-r1-cross-machine-replication.md, section 2) without ever treating missing or
# unreliable evidence as zero activity.
#
# usage: check_swap.sh --mode primary|smoke --before <vmstat-file> --after <vmstat-file>
#   run_replication.sh copies the live /proc/vmstat into its staging directory before and after the
#   workload and passes those copies; it offers no way to substitute other files.
#
# Each of pswpin and pswpout must appear exactly once per snapshot as "<name> <1-18 decimal digits>"
# (leading zeros are decimal). 18 digits keep every value and difference far inside signed 64-bit
# arithmetic; longer values are rejected before any arithmetic. A counter that decreases is invalid.
# Deltas are computed only after all four values are valid.
#
# Outcome (stdout, key=value; written by the runner to swap_activity.txt):
#   swap_activity=zero      VALID_ZERO_ACTIVITY      exit 0
#   swap_activity=positive  VALID_POSITIVE_ACTIVITY  exit 1 in primary mode
#   swap_activity=unknown   INVALID_EVIDENCE         exit 2 in primary mode (deltas reported as "invalid")
# In smoke mode positive and unknown exit 0 with a warning; unknown is never reported as zero.
# Usage errors exit 3.
set -u
MODE=""; BEFORE=""; AFTER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE=${2:-}; shift ;;
    --before) BEFORE=${2:-}; shift ;;
    --after) AFTER=${2:-}; shift ;;
    *) echo "check_swap.sh: unknown argument $1" >&2; exit 3 ;;
  esac
  shift
done
case "$MODE" in primary|smoke) ;; *) echo "check_swap.sh: --mode primary|smoke required" >&2; exit 3 ;; esac
[ -n "$BEFORE" ] && [ -n "$AFTER" ] || { echo "check_swap.sh: --before and --after required" >&2; exit 3; }

REASON=""
# read_counter <file> <name>: prints the decimal value, or sets REASON and prints nothing.
read_counter() {
  local file=$1 name=$2 n v
  if [ ! -r "$file" ]; then REASON="cannot read $file"; return 1; fi
  n=$(grep -cE "^$name([[:space:]]|$)" "$file" 2>/dev/null || true)
  if [ "$n" != 1 ]; then REASON="$name appears $n times in $(basename "$file") (expected exactly once)"; return 1; fi
  v=$(sed -n "s/^$name[[:space:]]\{1,\}\([0-9]\{1,18\}\)[[:space:]]*\$/\1/p" "$file")
  if [ -z "$v" ]; then REASON="$name in $(basename "$file") is empty, non-numeric, negative or longer than 18 digits"; return 1; fi
  printf '%s\n' "$((10#$v))"
}

valid=1
in0=$(read_counter "$BEFORE" pswpin) || valid=0
[ "$valid" -eq 1 ] && { out0=$(read_counter "$BEFORE" pswpout) || valid=0; }
[ "$valid" -eq 1 ] && { in1=$(read_counter "$AFTER" pswpin) || valid=0; }
[ "$valid" -eq 1 ] && { out1=$(read_counter "$AFTER" pswpout) || valid=0; }
# REASON is set inside command substitutions (subshells); re-derive it for the report.
if [ "$valid" -eq 0 ]; then
  for pair in "$BEFORE:pswpin" "$BEFORE:pswpout" "$AFTER:pswpin" "$AFTER:pswpout"; do
    read_counter "${pair%:*}" "${pair##*:}" > /dev/null || break
  done
elif [ "$in1" -lt "$in0" ] || [ "$out1" -lt "$out0" ]; then
  valid=0; REASON="a counter decreased between snapshots (pswpin $in0 -> $in1, pswpout $out0 -> $out1)"
fi

if [ "$valid" -eq 0 ]; then
  echo "swap_evidence=invalid"
  echo "pswpin_delta=invalid"
  echo "pswpout_delta=invalid"
  echo "swap_activity=unknown"
  echo "reason=$REASON"
  if [ "$MODE" = primary ]; then
    echo "REFUSED: swap evidence from /proc/vmstat is missing or unreliable ($REASON); the preregistered 'no active swap' cannot be verified" >&2
    exit 2
  fi
  echo "WARNING: swap evidence is missing or unreliable ($REASON); this smoke run has NO verified zero-swap measurement." >&2
  exit 0
fi

din=$((in1 - in0)); dout=$((out1 - out0))
echo "swap_evidence=valid"
echo "pswpin_before=$in0"; echo "pswpin_after=$in1"; echo "pswpin_delta=$din"
echo "pswpout_before=$out0"; echo "pswpout_after=$out1"; echo "pswpout_delta=$dout"
if [ "$din" -eq 0 ] && [ "$dout" -eq 0 ]; then
  echo "swap_activity=zero"
  exit 0
fi
echo "swap_activity=positive"
if [ "$MODE" = primary ]; then
  echo "REFUSED: swap activity during the run (pswpin_delta=$din, pswpout_delta=$dout); the preregistration requires no active swap" >&2
  exit 1
fi
echo "WARNING: swap activity during this smoke run (pswpin_delta=$din, pswpout_delta=$dout)." >&2
exit 0

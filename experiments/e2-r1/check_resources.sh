#!/bin/bash
# E2-R1 resource preflight: enforces the preregistered "at least 8 GiB of RAM available"
# (docs/experiments/e2-r1-cross-machine-replication.md, section 2) before any expensive work.
#
# usage: check_resources.sh --mode primary|smoke [--meminfo <file>]
#   --meminfo exists only so the tests can feed fixtures; run_replication.sh never passes it,
#   so a scientific run always reads the live /proc/meminfo.
#
# Source and unit: MemAvailable from /proc/meminfo. The kernel prints it as "<n> kB", where n is
# pages << (PAGE_SHIFT - 10), i.e. units of 1024 bytes (fs/proc/meminfo.c, show_val_kb). So
# 8 GiB = 8 * 1024 * 1024 = 8388608 "kB". Swap is not RAM and is never added.
#
# exit 0: primary and MemAvailable >= 8 GiB (RESOURCES=ok), or smoke at any MemAvailable
#         (RESOURCES=ok, or RESOURCES=non-primary with a warning when below the threshold)
# exit 1: primary and MemAvailable < 8 GiB (RESOURCES=refused)
# exit 2: usage error, or MemAvailable missing, duplicated, unreadable or malformed, in either mode
#         (RESOURCES=unknown): no memory evidence is never treated as enough memory.
set -u
REQUIRED_KIB=8388608
MODE=""; MEMINFO=/proc/meminfo
while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE=${2:-}; shift ;;
    --meminfo) MEMINFO=${2:-}; shift ;;
    *) echo "check_resources.sh: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
case "$MODE" in primary|smoke) ;; *) echo "check_resources.sh: --mode primary|smoke required" >&2; exit 2 ;; esac

gib() { awk -v k="$1" 'BEGIN { printf "%.2f", k / 1048576 }'; }

if [ ! -r "$MEMINFO" ]; then
  echo "RESOURCES=unknown"
  echo "REFUSED: cannot read $MEMINFO; the preregistered >= 8 GiB MemAvailable requirement cannot be verified" >&2
  exit 2
fi
# Exactly one line of the exact form "MemAvailable: <digits> kB"; anything else is malformed.
lines=$(grep -c '^MemAvailable:' "$MEMINFO" 2>/dev/null || true)
value=$(sed -n 's/^MemAvailable:[[:space:]]\{1,\}\([0-9]\{1,18\}\)[[:space:]]\{1,\}kB[[:space:]]*$/\1/p' "$MEMINFO")
if [ "$lines" != 1 ] || [ -z "$value" ] || [ "$(printf '%s\n' "$value" | wc -l)" -ne 1 ]; then
  echo "RESOURCES=unknown"
  echo "REFUSED: MemAvailable missing or malformed in $MEMINFO; the preregistered >= 8 GiB MemAvailable requirement cannot be verified" >&2
  exit 2
fi
value=$((10#$value))   # strip leading zeros without octal interpretation
echo "MEMAVAILABLE_KIB=$value"
echo "REQUIRED_KIB=$REQUIRED_KIB"

if [ "$value" -ge "$REQUIRED_KIB" ]; then
  echo "RESOURCES=ok"
  exit 0
fi
if [ "$MODE" = primary ]; then
  echo "RESOURCES=refused"
  echo "REFUSED: E2-R1 primary requires at least 8 GiB MemAvailable ($REQUIRED_KIB kB in /proc/meminfo); host reports $value kB ($(gib "$value") GiB). This is a preregistered requirement (e2-r1-cross-machine-replication.md, section 2); swap does not satisfy it." >&2
  exit 1
fi
echo "RESOURCES=non-primary"
echo "WARNING: NON-PRIMARY TOOLING VALIDATION. MemAvailable is $value kB ($(gib "$value") GiB), below the preregistered 8 GiB ($REQUIRED_KIB kB) required for E2-R1 primary data. The smoke run is allowed only because it is not R1 primary data; its SMOKE- bundle must stay excluded from scientific analysis." >&2
exit 0

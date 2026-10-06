#!/bin/bash
# Codegen report (secondary) for every installed compiler on this host.
# usage: generate_codegen_report.sh <repo-root> <out-dir>
set -u
ROOT=$1; OUT=$2
mkdir -p "$OUT"
for pair in "g++:gcc" "clang++:clang"; do
  cxx=${pair%%:*}; label=${pair##*:}
  if command -v "$cxx" >/dev/null 2>&1; then
    "$ROOT/experiments/e2-materialize-vs-recompute/codegen_report.sh" "$ROOT" "$OUT" "$cxx" "$label-$(uname -m)" > /dev/null || echo "codegen report failed for $cxx" >&2
    rm -f "$OUT/$label-$(uname -m).o" "$OUT/$label-$(uname -m).s"   # keep the disassembly, vectorization log and report
  else
    echo "$cxx not installed" > "$OUT/$label-$(uname -m).missing"
  fi
done

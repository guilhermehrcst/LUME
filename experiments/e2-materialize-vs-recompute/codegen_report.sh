#!/bin/bash
# Compile the executor TU with the exact primary flags and report vectorization + tight-loop structure.
# usage: codegen_report.sh <repo-root> <out-dir> <cxx-compiler> <label>
set -eu
ROOT=$1; OUT=$2; CXX=$3; LABEL=$4
mkdir -p "$OUT"
INC="-I$ROOT/include -I$ROOT/src/runtime"
FLAGS="-O3 -DNDEBUG -std=c++20"       # primary configuration: no -march=native, no -ffast-math
if "$CXX" --version | grep -qi clang; then
  VEC="-Rpass=loop-vectorize -Rpass=loop-unroll -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize"
else
  VEC="-fopt-info-vec-all"
fi
"$CXX" $FLAGS $INC -c "$ROOT/src/runtime/cpu_reference.cpp" -o "$OUT/$LABEL.o" $VEC 2> "$OUT/$LABEL.vec.txt"
"$CXX" $FLAGS $INC -S "$ROOT/src/runtime/cpu_reference.cpp" -o "$OUT/$LABEL.s"
if [ "$(uname -m)" = x86_64 ]; then objdump -d -C --no-show-raw-insn -M intel "$OUT/$LABEL.o" > "$OUT/$LABEL.dis"; else objdump -d -C --no-show-raw-insn "$OUT/$LABEL.o" > "$OUT/$LABEL.dis"; fi
{ echo "compiler: $("$CXX" --version | head -1)"; echo "flags: $FLAGS"; echo "arch: $(uname -m)"; } > "$OUT/$LABEL.meta"
python3 "$(dirname "$0")/codegen_report.py" "$OUT/$LABEL.dis" "$LABEL" > "$OUT/$LABEL.report.md"
cat "$OUT/$LABEL.report.md"

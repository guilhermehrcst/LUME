# PXIR

**Experimental intermediate representation research for memory-efficient software and AI systems.**

> Status: early research. PXIR currently makes no production or universal performance claims.

PXIR is a public systems-research project exploring whether software, structured data, and computational intent can be represented with less memory, less redundant data movement, and lower processing overhead while preserving correctness.

The project starts from measurable fundamentals rather than from a finished compiler design.

## Research question

Can we represent software, data, and computation more efficiently in memory, storage, movement, and processing without losing the semantics required by the workload?

## Method

PXIR follows one loop:

**Understand -> Implement -> Measure -> Explain -> Publish**

Core rules:

1. No optimization claim without measurement.
2. No measurement without reproducibility.
3. No representation change without correctness checks.
4. Establish a baseline before optimizing.
5. Publish negative results and trade-offs too.
6. Treat CI as a correctness gate, not as a trustworthy performance laboratory.

## Current scope

The first phase studies memory representation directly:

- primitive type width;
- allocation overhead;
- alignment and padding;
- cache locality;
- Array of Structures vs Structure of Arrays;
- bit packing;
- dictionary encoding;
- serialization;
- zero-copy and data movement.

Only after those experiments will PXIR move toward compact IR design, SIMD, GPU execution, CUDA backends, and AI-oriented representations.

## Languages

| Role | Language |
| --- | --- |
| Low-level learning experiments | C |
| PXIR core and future runtime | C++20 |
| Benchmarks, datasets, analysis and tooling | Python 3 |
| NVIDIA GPU backend | CUDA C++ |

C exposes the machine. C++ builds the system. Python measures the system. CUDA C++ eventually takes selected workloads to NVIDIA GPUs.

## Experiment 001

The first experiment establishes a deliberately small baseline: how much representational memory do fixed-width integer types require for the same number of elements?

See [`experiments/001-data-types`](experiments/001-data-types/README.md).

## M0: minimal executable IR

M0 is the first executable PXIR foundation. It represents `C = A + B` over `f32[N]` in a compact IR, verifies it, executes it on a scalar CPU reference executor, and checks the result against an independent native baseline.

```cpp
pxir::Program program;
auto a = program.input(pxir::f32, 1024);
auto b = program.input(pxir::f32, 1024);
auto c = program.add(a, b);
program.output(c);
```

Debug dump (for humans only; this is not a PXIR language):

```text
%0 = input f32[1024]
%1 = input f32[1024]
%2 = add %0, %1
output %2
```

- **Supports:** `f32`/`i32` types, `input`/`add`/`output`, a mandatory verifier (only a `VerifiedProgram` can execute), a CPU scalar reference executor, positive and negative correctness tests, and a baseline benchmark.
- **Does not support:** a textual PXIR language or parser, LLVM, CUDA, SIMD, optimization passes, JIT, a tensor compiler, or AI workloads.

M0 makes no performance claim. See [`docs/m0-core-ir.md`](docs/m0-core-ir.md) for the design, the verifier invariants, the correctness policy, and the baseline measurements.

## M1: executor cost breakdown

M1 is observational and changes nothing about execution. It measures where the M0 reference executor spends its time on `C = A + B` for N from 1 to 4,194,304, using `pxir_bench_executor_breakdown`.

On the measurement machine (glibc 2.39), most of the large-N cost comes from re-faulting heap pages that the allocator returns to the OS between calls, rather than from the IR abstraction. The measured input validation and executor setup together cost about 0.12 µs per call. Interpretation and dispatch were not measured on their own: they are inferred to be small only because a replica with no interpreter matches the full executor within this experiment's noise. The single-machine methodology, the raw data, and what remains unknown are in [`docs/m1-executor-cost-breakdown.md`](docs/m1-executor-cost-breakdown.md).

## M2: move owned output on last use

M2 makes exactly one runtime change. The reference executor transfers an executor-owned result buffer into `ExecutionResult::outputs`, with no N-sized copy, when the output is that value's last use. Outputs of inputs, and outputs of values read again later, are still copied. This is not a zero-copy executor: the result buffer is still allocated and zero-initialized.

- **Model:** on the canonical `C = A + B; output C` path, user-level traffic falls from 24 to 16 B/element.
- **Measured** (interleaved A/B against M1, one machine with glibc 2.39): at N = 1,048,576 the executor went from 3.29 ms (2,016 page faults per call) to 0.60 ms (0 faults).
- **Allocator dependence:** with every large block forced through mmap, M2 still faults, about half as often as M1.
- **Small N:** the last-use table adds 20–23 ns in isolation.

Details and caveats are in [`docs/m2-output-move-last-use.md`](docs/m2-output-move-last-use.md).

## M3: single-write result construction (rejected; research record only)

M3 tested single-write result construction using standard C++20 `reserve` + `emplace_back`, instead of `std::vector<T>(n)` + indexed writes.

- **What it achieved:** it removed the explicit zero-fill and reduced the model from 16 to 12 B/element.
- **What went wrong:** GCC 13.3 and Clang 18.1 both failed to vectorize the append loop.
- **Measured at N = 1,048,576** (interleaved A/B, one machine):
  - M2: about 0.63 ms;
  - M3 with GCC: about 1.08 ms;
  - M3 with Clang: about 1.21× M2.
- **Status:** M3 was therefore rejected as canonical runtime behavior. The current executor keeps M2's result construction; M3's code exists only in the experimental commit `2888c0a`.

Details, raw data and the reproduction reference are in [`docs/m3-single-write-result.md`](docs/m3-single-write-result.md).

## M4: vectorizable single-write owned storage

M4 replaces the zero-filled `std::vector` result with `OwnedArray<T>`, storage created by `std::make_unique_for_overwrite<T[]>` whose elements are not value-initialized. It keeps M2's plain indexed loop, and every element is written exactly once before it can be read. GCC 13.3 and Clang 18.1 vectorize the loop in isolation and in the integrated executor.

Measured in an interleaved A/B against M2 on one machine, N = 1,048,576:
- M2 ≈ 0.62 ms and M4 ≈ 0.47 ms, with GCC; M4/M2 ≈ 0.74 with Clang;
- M4 is about equal to the native preallocated loop;
- the model falls from 16 to 12 B/element.

With Clang, small N shows a regression of about 9 ns at N = 1.

M4 changes the public `Buffer` API: `as_f32`/`as_i32` are replaced by `f32_view`/`i32_view` (`std::optional<std::span<const T>>`). Details, evidence and the merge conditions are in [`docs/m4-vectorizable-owned-storage.md`](docs/m4-vectorizable-owned-storage.md).

## M5: multi-operation / intermediate-materialization baseline

M5 is observational: the runtime is unchanged and no fusion was implemented. It measures `D = A + B; E = D + C` through the canonical executor against native fused (`e[i] = (a[i] + b[i]) + c[i]`) and two-pass loops, and against an interpreter-free replica.

Measured on one machine at N = 1,048,576, relative to the native fused loop:
- **Steady state (glibc trim/mmap disabled):** the PXIR chain costs about the same as a native two-pass loop and is 1.54× the fused loop, close to the 24/16 B/element model.
- **Default glibc:** the chain is 4.3× the fused loop. Most of that comes from the allocator trimming and re-faulting the two result buffers (2,016 minor faults per call), not from the intermediate's extra bytes.
- **Outputting D before its later use** (a required copy) adds about 0.3 ms; outputting it after its last use (a move) adds almost nothing.

Details are in [`docs/m5-intermediate-materialization.md`](docs/m5-intermediate-materialization.md).

## M6: last-use in-place result reuse

M6 reuses executor-owned storage when an `add` consumes an operand for the last time: the result is computed inside that operand's buffer and ownership moves to the result value. It keeps two computational passes (this is not fusion) and the 24 B/element two-pass payload model; it removes one result allocation and lowers the logical peak (final-only chain: 8 to 4 B/element). Caller inputs are never reused, and a later output of a value blocks reuse. No IR, verifier or public API change.

Measured on one machine, GCC, N = 1,048,576, `D = A + B; E = D + C`:
- Default glibc: 2.90 ms to 0.80 ms and 2,016 to 0 minor faults per call.
- Forced mmap (T2): fault sets 2 to 1.
- Trim/mmap disabled (T1): 0.97 ms to 0.80 ms, which is not explained by payload (unchanged); a preallocated microbenchmark shows an in-place second pass at about 0.84x an out-of-place one.
- Controls that cannot reuse (single add, output after use) are unchanged in time and faults. Non-reusing programs are 3 to 5 % slower at N = 1 to 256.

Details, evidence and the merge conditions are in [`docs/m6-last-use-inplace-reuse.md`](docs/m6-last-use-inplace-reuse.md).

## Experiment 002: columnar encodings for an event-log workload

Experiment 002 is the first test of H1 and H2 on a workload that is not a vector add: an append-only event log held in memory as a plain typed columnar layout (B1), a dictionary-encoded one (B2) and a dictionary + bit-packing + delta one (E1). The hypothesis and thresholds were pre-registered before any measurement, and the layouts are lossless (checked by a round-trip test that was itself mutation-tested).

Measured (GCC 13.3 and Clang 18.1, one machine, synthetic calibrated workload, 200k to 3M rows):
- **Footprint (T1, passed):** E1 uses 0.215x the bytes of B1 (24.8 vs 115.2 B/row) and 0.576x those of B2.
- **Scan cost (T2, failed):** the worst scan is 3.3-4.2x B1, on a query that must rebuild every timestamp from deltas; point lookups are 1.4-1.7x slower.
- The pre-registered hypothesis is therefore **not supported**: the encodings trade time for memory on this implementation.

Details, raw data and limitations are in [`experiments/002-columnar-encodings`](experiments/002-columnar-encodings/README.md).

## Build

Requirements:

- CMake 3.20+
- a C11 compiler
- a C++20 compiler (GCC 13 and Clang 18 are tested locally; CI also builds with the macOS and Windows runner defaults)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Examples:

```bash
./build/experiments/001-data-types/pxir_exp_001 u32 1000000
./build/benchmarks/pxir_bench_vector_add            # [elements] [seed] [warmup] [iterations]
ctest --test-dir build -L correctness              # PXIR correctness tests only
```

Sanitizers and strict warnings (GCC/Clang):

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DPXIR_SANITIZE=address,undefined -DPXIR_WARNINGS_AS_ERRORS=ON
cmake --build build-san && ctest --test-dir build-san --output-on-failure
```

## Research documentation

- [`docs/research.md`](docs/research.md): research question, hypotheses, scope and non-goals.
- [`docs/benchmark-methodology.md`](docs/benchmark-methodology.md): rules for measurements and performance claims.
- [`docs/m0-core-ir.md`](docs/m0-core-ir.md): M0 IR design, verifier invariants, correctness policy and baseline.
- [`docs/m1-executor-cost-breakdown.md`](docs/m1-executor-cost-breakdown.md): M1 measurement of where the reference executor spends time.
- [`docs/m2-output-move-last-use.md`](docs/m2-output-move-last-use.md): M2 output move on last use, A/B against M1.
- [`docs/m3-single-write-result.md`](docs/m3-single-write-result.md): M3 single-write result construction, a rejected experiment (negative result).
- [`docs/m4-vectorizable-owned-storage.md`](docs/m4-vectorizable-owned-storage.md): M4 single-write owned storage that keeps auto-vectorization.
- [`docs/m5-intermediate-materialization.md`](docs/m5-intermediate-materialization.md): M5 intermediate-materialization baseline (observational).
- [`docs/m6-last-use-inplace-reuse.md`](docs/m6-last-use-inplace-reuse.md): M6 last-use in-place result reuse.
- [`experiments/002-columnar-encodings/README.md`](experiments/002-columnar-encodings/README.md): experiment 002, pre-registered columnar-encoding test (T1 passed, T2 failed).

## License

Apache License 2.0.

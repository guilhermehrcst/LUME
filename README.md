# Lume

**Experimental systems research for efficient representation, memory, data movement and adaptive computation.**

> Status: early research. Lume currently makes no production or universal performance claims.

> Formerly named PXIR. The code, namespaces, CMake targets and options were renamed to Lume; commit `b2354749f8331557300e746ae6edc5a01284dc4f` is the last one under the old name. See [docs/data/README.md](docs/data/README.md) for what was deliberately left unchanged.

Lume is a public systems-research project exploring whether software, structured data, and computational workloads can be represented and executed with less memory, less redundant data movement, and lower processing overhead while preserving correctness.

The project starts from measurable fundamentals rather than from a finished compiler, runtime, or hardware design. Lume is deliberately built from falsifiable experiments upward: representation first, then memory behavior, data movement, execution planning, heterogeneous backends, and only much later any case for custom hardware.

## Research question

Can we represent and execute software, data, and computation more efficiently in memory, storage, movement, and processing without losing the semantics required by the workload?

## Method

Lume follows one loop:

**Understand -> Implement -> Measure -> Explain -> Publish**

Core rules:

1. No optimization claim without measurement.
2. No measurement without reproducibility.
3. No representation change without correctness checks.
4. Establish a baseline before optimizing.
5. Publish negative results and trade-offs too.
6. Treat CI as a correctness gate, not as a trustworthy performance laboratory.

## Current state

Lume has already moved beyond its initial memory-only phase. The repository currently contains:

- a compact typed IR with deterministic contiguous storage;
- a mandatory verifier that produces immutable `VerifiedProgram` instances before execution;
- a CPU execution path that has evolved through ownership transfer, single-write storage, last-use reuse and single-use intermediate fusion;
- independent correctness oracles and negative tests;
- reproducible benchmarks with raw measurement evidence;
- representation experiments, including fixed-width data types and lossless columnar encodings;
- cross-platform CI on Linux, macOS and Windows, plus ASan + UBSan on Ubuntu.

The implementation is still intentionally narrow. Today Lume is CPU-first and supports only a small IR centered on `input`, `add` and `output`. It does **not** yet provide a production compiler, GPU backend, CUDA runtime, Lume Intent implementation, Lume Pixel pipeline, adaptive hardware fabric or custom silicon.

## Research architecture

Lume is evolving as one research platform with a shared core and several research lines. These names describe the direction of the program; they do not imply that every component is implemented today.

### Lume Core

The common foundation:

- typed IR and explicit semantics;
- verification before execution;
- cost, lifetime and dependency information;
- memory and execution planning;
- reproducible benchmark infrastructure;
- CPU and future heterogeneous backends.

### Lume Memory

Research into representation and data movement:

- layout and locality;
- allocation and buffer lifetime;
- zero-copy and buffer reuse;
- bit packing and dictionary encoding;
- serialization;
- quantization where semantics allow it;
- recomputation versus materialization;
- CPU/GPU transfer and memory locality.

### Lume Compute

Research into how verified computation should be scheduled and executed:

- CPU execution;
- fusion and dataflow;
- explicit lifetime/use analysis;
- execution planning;
- future GPU backends;
- future SIMD and CUDA experiments;
- longer-term simulation of adaptive compute fabrics.

The goal is not to copy a CPU or GPU. It is to investigate whether execution can be organized around the location, representation and lifetime of data instead of forcing every workload through a fixed sequence of materializations and transfers.

### Lume Intent

A future research line for translating human intent into a formal, constrained and verifiable plan before execution. Ambiguity and unsafe or contradictory plans should fail explicitly instead of being guessed through.

### Lume Pixel

A future domain-specific research line for efficient image and eventually video representation and processing, including tiles, partial processing, fused pipelines, layout-aware execution and reduced CPU/GPU data movement.

## North-star metric

A central Lume question is:

> **How many bytes must move to produce one useful unit of correct computation?**

A useful conceptual metric is therefore:

**Bytes Moved / Useful Operation**

It is not the only metric. Latency, throughput, peak memory, allocations, passes, cache behavior, transfer cost and energy still matter. Correctness remains the hard gate before any performance claim.

## Current direction

The current implementation naturally forms a sequence:

`copy -> allocation -> materialization -> reuse -> fusion -> dataflow -> placement`

M2 removed an unnecessary output copy. M4 removed unnecessary result initialization while keeping vectorization. M5 isolated intermediate materialization cost. M6 reused storage on last use. M7 eliminated a strictly single-use intermediate through fusion.

The next candidate is **M8: maximal single-use Add-chain fusion**, extending M7 from one adjacent pair to a whole eligible chain while keeping the same conservative correctness and allocation/fault invariants.

In parallel, Lume should make use/lifetime analysis and logical data movement increasingly explicit so later execution planning can be measured instead of inferred. GPU work comes after a CPU-side hypothesis and baseline exist; CUDA is a future backend, not the definition of Lume.

## Languages

| Role | Language |
| --- | --- |
| Low-level learning experiments | C |
| Lume core and runtime | C++20 |
| Benchmarks, datasets, analysis and tooling | Python 3 |
| NVIDIA GPU backend | CUDA C++ |

C exposes the machine. C++ builds the system. Python measures and orchestrates experiments. CUDA C++ is reserved for future NVIDIA GPU work after CPU-side hypotheses and baselines are established.

## Experiment 001

The first experiment establishes a deliberately small baseline: how much representational memory do fixed-width integer types require for the same number of elements?

See [`experiments/001-data-types`](experiments/001-data-types/README.md).

## M0: minimal executable IR

M0 is the first executable Lume foundation. It represents `C = A + B` over `f32[N]` in a compact IR, verifies it, executes it on a scalar CPU reference executor, and checks the result against an independent native baseline.

```cpp
lume::Program program;
auto a = program.input(lume::f32, 1024);
auto b = program.input(lume::f32, 1024);
auto c = program.add(a, b);
program.output(c);
```

Debug dump (for humans only; this is not a Lume language):

```text
%0 = input f32[1024]
%1 = input f32[1024]
%2 = add %0, %1
output %2
```

- **Supports:** `f32`/`i32` types, `input`/`add`/`output`, a mandatory verifier (only a `VerifiedProgram` can execute), a CPU scalar reference executor, positive and negative correctness tests, and a baseline benchmark.
- **Does not support:** a textual Lume language or parser, LLVM, CUDA, SIMD, optimization passes, JIT, a tensor compiler, or AI workloads.

M0 makes no performance claim. See [`docs/m0-core-ir.md`](docs/m0-core-ir.md) for the design, the verifier invariants, the correctness policy, and the baseline measurements.

## M1: executor cost breakdown

M1 is observational and changes nothing about execution. It measures where the M0 reference executor spends its time on `C = A + B` for N from 1 to 4,194,304, using `lume_bench_executor_breakdown`.

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
- **Steady state (glibc trim/mmap disabled):** the Lume chain costs about the same as a native two-pass loop and is 1.54× the fused loop, close to the 24/16 B/element model.
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

## M7: single-use intermediate fusion

M7 fuses a strictly eligible adjacent `add` → `add` pair whose intermediate has no observable use, eliminating its materialized buffer while keeping one result allocation. For `T = A + B; R = T + C` (or `C + T`) the executor computes `t = a[i] + b[i]` and then `r[i] = t + c[i]` in one loop; `T` stays in the IR but never gets a buffer. The grouping is kept as written (no reassociation). A pair is not fused when `T` is output or read again, when the two adds are not adjacent, when `T` is used twice in the second add, or when M6 could already compute either add in an operand's storage, so the allocation count stays exactly M6's. No IR, verifier or public API change.

Measured on one machine (shared VM), `D = A + B; E = D + C`, paired A/B against M6:
- N = 1,048,576: 0.82 ms to 0.67 ms under default glibc (0.81x), 0.81 ms to 0.65 ms with trim/mmap disabled (0.82x); about 1.00x the native fused loop, where M6 was about 1.24x. Clang agrees.
- Allocations, page faults (default, trim disabled, forced mmap) and allocator syscalls are identical to M6, so the saving is not an allocator effect.
- The naive payload ratio 16/24 = 0.67 was not reached; M6's in-place second pass already cost less than its model.
- Programs that are not fused are unchanged; no small-N overhead was detected. Chains of 3 or 4 adds fuse only the first pair and remain 1.24 to 1.39x a native single loop.

Details, evidence and the merge conditions are in [`docs/m7-single-use-intermediate-fusion.md`](docs/m7-single-use-intermediate-fusion.md).

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
./build/experiments/001-data-types/lume_exp_001 u32 1000000
./build/benchmarks/lume_bench_vector_add            # [elements] [seed] [warmup] [iterations]
ctest --test-dir build -L correctness              # Lume correctness tests only
```

Sanitizers and strict warnings (GCC/Clang):

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DLUME_SANITIZE=address,undefined -DLUME_WARNINGS_AS_ERRORS=ON
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
- [`docs/m7-single-use-intermediate-fusion.md`](docs/m7-single-use-intermediate-fusion.md): M7 single-use intermediate fusion.
- [`docs/experiments/e1-report.md`](docs/experiments/e1-report.md): E1, is execution planning necessary in the current plan space? (preregistration: [`e1-is-planning-necessary.md`](docs/experiments/e1-is-planning-necessary.md))
- [`experiments/002-columnar-encodings/README.md`](experiments/002-columnar-encodings/README.md): experiment 002, pre-registered columnar-encoding test (T1 passed, T2 failed).

## License

Apache License 2.0.

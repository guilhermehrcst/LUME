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

## License

Apache License 2.0.

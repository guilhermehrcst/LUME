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

## Build

Requirements:

- CMake 3.20+
- a C11 compiler
- later phases will require a C++20 compiler

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Example:

```bash
./build/experiments/001-data-types/pxir_exp_001 u32 1000000
```

## Research documentation

- [`docs/research.md`](docs/research.md): research question, hypotheses, scope and non-goals.
- [`docs/benchmark-methodology.md`](docs/benchmark-methodology.md): rules for measurements and performance claims.

## License

Apache License 2.0.

# Experiment 001: Fixed-width integer representation

## Question

How much representational memory is required to store the same sequence of values using 8-bit, 16-bit, 32-bit, and 64-bit unsigned integers?

## Why this experiment exists

PXIR begins with the smallest useful baseline. Before discussing compression, IR design, caches, or GPUs, we need a precise understanding of the relationship between value range and representation width.

## Hypothesis

For a fixed element count, requested storage should scale linearly with the width of the selected integer type.

For 1,000,000 elements, ignoring allocator and process overhead:

| Type | Bytes per element | Requested bytes |
| --- | ---: | ---: |
| uint8_t | 1 | 1,000,000 |
| uint16_t | 2 | 2,000,000 |
| uint32_t | 4 | 4,000,000 |
| uint64_t | 8 | 8,000,000 |

All four variants write the same logical value sequence, `i % 251`, so every value fits in every tested integer width.

The experiment must not confuse requested representation bytes with total process RSS.

## Build

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Run

On Unix-like systems:

```bash
./build/experiments/001-data-types/pxir_exp_001 u8 1000000
./build/experiments/001-data-types/pxir_exp_001 u16 1000000
./build/experiments/001-data-types/pxir_exp_001 u32 1000000
./build/experiments/001-data-types/pxir_exp_001 u64 1000000
```

The executable allocates the requested typed array, writes every element through volatile accesses so the pages are actually touched, reads the values back into a checksum, and prints the requested representation size.

## Measure process memory

Linux example:

```bash
/usr/bin/time -v ./build/experiments/001-data-types/pxir_exp_001 u32 1000000
```

macOS example:

```bash
/usr/bin/time -l ./build/experiments/001-data-types/pxir_exp_001 u32 1000000
```

Record the environment before comparing RSS results. Process overhead means RSS will not equal requested bytes exactly.

## Correctness

The program rejects zero, negative, non-numeric counts, unknown types, integer-overflow risk, and allocation failure. CTest verifies all supported widths and negative CLI cases.

## Expected conclusion

This experiment should establish a baseline, not a novel result. Its purpose is to make later PXIR memory claims precise and reproducible.

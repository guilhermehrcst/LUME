# E2 - next architecture (conditional; nothing here is implemented)

Status: **conditional on physical-machine replication of E2** (x86 and, if available, ARM64). E2 was measured on one KVM guest. This document lists only the facts E2 showed to matter; it does not authorize implementation.

## Pipeline that E2 would justify

```text
VerifiedProgram -> Facts -> candidate plans -> CostModel -> selected plan -> verify_plan -> execute
```

## Facts E2 showed to matter (MEASURED on one VM)

| fact | evidence |
| --- | --- |
| fan-out k of a shared add intermediate | decides at large N: R for k <= 2, M for k >= 4 mostly follows the byte model |
| element count N (static in the IR) | the winner for the same (dtype, k) flips with N in 36-50 stable pairs per compiler, up to 1.31x; non-monotone |
| dtype | matters at k = 1 (i32 M/R up to 1.41 versus f32 1.17) |
| operand producers are caller inputs | precondition of the E2 recompute seam, not a performance fact |
| compiler/toolchain | best global T is 1 (GCC) vs 2 (Clang); calibration must be per toolchain |

## Facts E2 did NOT show to matter

- memory regime (arena vs default glibc, warm vs cold): 1 pure-regime flip per compiler; per-(dtype,k,N) tables ignoring regime have p95 regret 1.005-1.006 (post hoc, oracle).
- allocator state, page faults (equal for M and R by construction).

## Smallest shape consistent with the evidence

A per-machine, per-toolchain calibration table keyed on (dtype, fan-out, N class) consulted by the strategy choice, produced offline by a benchmark like `lume_e2_bench`, and **plan verification as a separate oracle-checked step** (the E2 tests already compare every output with the independent oracle). A runtime cost model is not supported by this data.

## Required before any of this

1. Replicate the E2 matrix on physical x86 and ARM64 machines (no VM) with a PMU, to identify whether the non-monotone N profile is a cache-capacity effect.
2. Inspect generated code of the two loops to separate loop-shape from memory-traffic effects.
3. Only then decide between calibration tables, a `CostModel`, or stopping the planner thesis.

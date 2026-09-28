# PXIR Research Charter

## Central question

Can software, structured data, and computational intent be represented significantly more efficiently in memory, storage, movement, and processing while preserving the semantics required by the workload?

PXIR treats this as a research question, not as an assumption.

## Initial hypotheses

### H1: representation density

For selected workloads, a more compact representation can reduce memory footprint without changing the information required by the workload.

### H2: data movement

For memory-bound workloads, reducing unnecessary copies and improving layout can matter as much as, or more than, reducing serialized size.

### H3: layout-aware execution

Representations that expose useful layout and dependency information may enable better CPU cache behavior, vectorization, or later GPU execution.

### H4: AI-oriented representation

A future compact semantic representation of code may reduce token footprint for some AI workloads, but usefulness must be measured by preserved task performance, not token count alone.

## What would falsify a hypothesis?

A PXIR idea fails for a workload when the measured trade-off is not worthwhile. Examples include:

- memory savings that cause unacceptable CPU overhead;
- smaller encodings whose decode cost dominates the workload;
- compact layouts that make required random access slower;
- token reductions that reduce model understanding or correctness;
- GPU layouts whose conversion or transfer cost exceeds execution savings.

A negative result is a valid research result and should be documented.

## Current scope

Phase 1 focuses on foundational memory behavior:

1. fixed-width data types;
2. allocation overhead;
3. alignment and padding;
4. cache locality;
5. AoS vs SoA;
6. bit packing;
7. dictionary encoding;
8. serialization;
9. zero-copy and memory mapping.

Later phases may investigate compact IRs, compiler transformations, SIMD, heterogeneous execution, CUDA, and AI workloads.

## Non-goals at this stage

PXIR is not currently:

- a replacement for CUDA;
- a production compiler;
- a new programming language;
- a universal compression format;
- a claim that smaller representations are always faster;
- a claim that one representation should fit every workload.

## Language strategy

- **C** for experiments where direct visibility into memory is the point.
- **C++20** for the future PXIR core, IR, runtime, and performance-critical code.
- **Python 3** for benchmark orchestration, dataset generation, analysis, and future bindings.
- **CUDA C++** for a future NVIDIA GPU backend after CPU-side fundamentals are established.

## Research invariant

Correctness comes before compression, speed, elegance, or benchmark results.

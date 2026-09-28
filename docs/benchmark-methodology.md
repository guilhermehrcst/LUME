# Benchmark Methodology

PXIR performance results must be reproducible enough for another engineer to rerun the experiment and understand its limitations.

## Required experiment structure

Every performance experiment should record:

1. question;
2. hypothesis;
3. environment;
4. dataset or input generator;
5. baseline;
6. candidate implementation;
7. correctness check;
8. measurement procedure;
9. raw or machine-readable results when practical;
10. analysis;
11. limitations;
12. conclusion.

## Environment

Record at minimum:

- operating system and version;
- CPU model;
- RAM;
- compiler and version;
- compiler flags;
- PXIR commit SHA;
- dataset size;
- relevant runtime or library versions.

GPU experiments must additionally record GPU model, VRAM, driver version, CUDA toolkit version, and relevant power/performance settings.

## Measurement rules

- Use a baseline before evaluating an optimization.
- Run correctness checks before accepting performance results.
- Prefer Release/optimized builds for performance measurements.
- Use multiple iterations when timing short operations.
- Report a robust statistic such as the median rather than a single convenient run.
- Record variability when it is material.
- Separate warm-up from measured runs when warm-up changes behavior.
- Change one important variable at a time when possible.
- Distinguish requested allocation size from process RSS and peak RSS.
- Distinguish serialized size from in-memory representation size.
- Distinguish latency from throughput.
- Do not compare results from materially different environments as though they were directly equivalent.

## CI policy

GitHub-hosted CI runners are useful for:

- compilation;
- tests;
- sanitizers where configured;
- format/static checks;
- smoke execution.

They are not the canonical source for performance claims because shared virtualized runners introduce noise and hardware drift.

## Result language

Prefer:

> On environment X and workload Y, candidate Z reduced measured peak RSS from A to B across N runs.

Avoid:

> PXIR uses 50% less memory.

unless the scope and evidence genuinely justify such a broad statement.

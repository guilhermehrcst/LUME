# Lume M1: Executor Cost Breakdown

M1 is observational. It changes nothing about how Lume executes. It measures where the M0 scalar reference executor spends its time on `C = A + B`, so that later milestones optimize measured costs instead of guessed ones.

Every number here comes from one machine (described below). None of it is a general performance claim about Lume.

## 1. Research question

When the M0 reference executor runs `C = A + B` over `f32[N]`, where does its time go? And which of those costs belong to the IR abstraction (validation, bookkeeping, dispatch), and which to memory management and result materialization?

Secondary questions:

1. What does runtime input validation cost?
2. What does executor bookkeeping cost?
3. What does creating the result buffer cost?
4. What does the scalar add loop cost?
5. What does copying the output cost?
6. How much of the native-vs-Lume gap do these explain?
7. Which costs scale with N, and which stay constant?

## 2. Canonical M0 baseline

M0 main is `381159e`. With `lume_bench_vector_add` at N = 1,048,576, seed 42, 5 warmup and 51 iterations, native is about 0.44–0.49 ms and Lume execution about 3.2–3.8 ms. In an interleaved A/B of main against this branch, the 6-run medians were 3.45 ms (main) and 3.48 ms (branch). Run-to-run spread for a single build was about ±10%.

The two paths do different work, so the ~7× ratio was not a result about Lume. Explaining it is the purpose of M1.

## 3. Hypotheses (stated before measuring)

| Id | Hypothesis | Outcome |
| --- | --- | --- |
| H1 | The scalar arithmetic loop is not responsible for most of the ~7× difference. | **Supported.** The executor-form add loop costs the same as the native loop (456 vs 468 µs at N = 1M). |
| H2 | Result allocation/initialization and output copying contribute substantially. | **Supported, but the mechanism was not the one expected.** In isolation, zero-fill and copy cost 145 + 299 µs, about 13% of the total. The dominant cost is re-faulting pages that glibc returns to the OS between calls. That comes from how the executor's buffer lifetimes interact with the allocator, and no isolated component captures it (§11). |
| H3 | For large N, memory traffic dominates fixed interpreter dispatch. | **Supported.** Measured: input validation plus executor setup total about 0.12 µs, against milliseconds of memory work. Inferred, not measured: interpretation and dispatch are also small, because `data_path_replica` (no interpreter) matches `pxir_execution_total` within noise. Dispatch was not isolated and has no measured value. |
| H4 | For very small N, validation-independent bookkeeping and dispatch become proportionally important. | **Supported, with qualifications.** On the batched N = 1 path (K = 1000, §4), measured validation plus setup is about 80% of the per-call time. The largest measured fixed cost is input validation (88 ns), and about 61 ns of it is associated with building diagnostic strings even when the input is valid. Dispatch was not measured separately, so it can't be ranked against validation. |

## 4. Experimental methodology

Harness: `benchmarks/executor_breakdown.cpp`, which builds `lume_bench_executor_breakdown`. It has no external dependencies. Timing uses `std::chrono::steady_clock`.

- **Sample.** One sample is one steady_clock interval around **K** back-to-back repetitions. The per-op time is the interval divided by K. For each (N, component) there are 5 warmup samples and 51 measured samples, and the reported statistic is the median; min and max are recorded too. The mean is never used.
- **Batching.** K = 1000 for operations that don't depend on N (validation, setup, message replica), and for every component when N < 4096. Otherwise K = 1, so large buffers see the allocator the way a single executor call does.
- **Lifetimes under batching.** When K = 1000, every result produced in an interval is kept until the interval ends. For `pxir_execution_total` that means 1,000 `ExecutionResult`s, and their output buffers, are alive at once. In a one-call-at-a-time loop, each result would be destroyed before the next call. The allocator state for small N (N = 1 and N = 256) is therefore not the same as canonical one-call execution. This doesn't affect the main large-N finding, because N ≥ 4,096 uses K = 1. N = 1 and N = 256 remain useful for showing the scale of fixed costs, but their exact allocation proportions should not be read as one-call allocator behaviour. The interval overhead is measured by `timer_overhead` at 26–30 ns. That is negligible when divided by K = 1000, but noticeable for K = 1 operations under about 1 µs.
- **No indirect calls while timing.** The timed operation is a template argument, so the timed loop contains no `std::function`.
- **Dead-code elimination is prevented:**
  - Every produced result (vectors, `Buffer`s, `ExecutionResult`s) goes into a pre-sized holder that outlives the interval.
  - After the interval, those results are checked against the oracle, untimed. The compiler must therefore produce them.
  - Allocations that produce nothing observable (the `executor_setup` replica, validation strings) write their address to a `const void* volatile` sink. Only the pointer is volatile; no array is.
  - No checksum or comparison runs inside a timed interval.
  - Evidence that the work actually happens: the timings scale with N as expected, and page-fault counts move with the allocator configuration.
- **Page faults.** Minor page faults come from `getrusage(RUSAGE_SELF).ru_minflt`, read just outside each interval, and are reported as the median per op.
- **Order.** Components run in a fixed order (forward). One full run used reverse order to expose order effects (§12).
- **Real code where possible.** `input_validation` times the executor's actual validation code. M1 extracted it, unchanged, into the internal `lume::detail::validate_inputs` (`src/runtime/input_validation.hpp`, not a public header). The other components are isolated equivalents of executor stages.
- **No instrumentation.** No timers were added to the executor, so there is no instrumentation overhead to report. `pxir_execution_total` is the unmodified executor.
- **Allocator experiments.** These change only glibc's malloc tunables, through the environment, never code:
  - T1: `GLIBC_TUNABLES=glibc.malloc.trim_threshold=1073741824:glibc.malloc.mmap_threshold=33554432` (never trim, never mmap).
  - T2: `GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072` (every block of 128 KiB or more comes from mmap).
- **Traced mechanism.** `strace -e trace=brk,mmap,munmap,madvise` over 10 executor calls at N = 1M.

Reproduce (Linux, glibc):

```bash
git rev-parse HEAD
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/benchmarks/lume_bench_executor_breakdown                      # forward, sizes 1..4194304
./build/benchmarks/lume_bench_executor_breakdown order=reverse
GLIBC_TUNABLES=glibc.malloc.trim_threshold=1073741824:glibc.malloc.mmap_threshold=33554432 \
  ./build/benchmarks/lume_bench_executor_breakdown sizes=65536,1048576,4194304
GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072 \
  ./build/benchmarks/lume_bench_executor_breakdown sizes=65536,1048576,4194304
```

The raw outputs of every run cited here are in [`docs/data/m1/`](data/m1/).

## 5. Environment

| Item | Value |
| --- | --- |
| Machine | Cloud container (Docker) on a shared VM; 4 vCPUs, 1 thread per core, 16 GiB RAM |
| CPU | Intel Xeon Processor @ 2.10 GHz (the model string is masked by the hypervisor); supports AVX2 and AVX-512F |
| Caches (per `lscpu`) | L1d 48 KiB/core, L2 2 MiB/core, **L3 260 MiB shared** |
| OS | Linux 6.18.44, 4 KiB pages, transparent huge pages `madvise` (not `always`); no cpufreq governor visible |
| libc | glibc 2.39 (Ubuntu), default malloc settings unless stated |
| Compilers | GCC 13.3.0 (canonical); Clang 18.1.3 (cross-check) |
| Flags | CMake Release: `-O3 -DNDEBUG`, with no `-march`, so the target is baseline x86-64 (SSE2) |
| Commit | This branch, which is `381159e` plus M1 changes |

The working set fits within the reported L3 capacity (12 MiB of kernel traffic at N = 1M, 48 MiB at N = 4M), and the observed rates of about 27 GB/s are consistent with cache-resident execution. Actual cache-hierarchy and DRAM traffic were not measured: there were no hardware performance counters, and a virtualized L3 may be shared or partitioned in ways `lscpu` doesn't show. The rates are the streaming rates this VM's memory hierarchy delivered, and this experiment can't attribute them to a specific level.

## 6. Workloads

`C = A + B`, `f32[N]`, for N ∈ {1, 256, 4,096, 65,536, 1,048,576, 4,194,304}. Inputs are deterministic: `mt19937_64(seed = 42)`, A then B, mapped to exact floats in [−1, 1), the same as M0.

## 7. Component definitions

The executor stages below were confirmed by reading `src/runtime/cpu_reference.cpp` at `381159e`.

| Component | What is timed | Executor stage it corresponds to |
| --- | --- | --- |
| `native_add_preallocated` | `lume_oracle::native_add(a, b, c)`, with C allocated and first touched once, untimed | None; this is the baseline |
| `add_loop_preallocated` | The executor's loop form (`c[i] = a[i] + b[i]` over `std::vector`), reading the `Buffer` inputs, C preallocated | The loop inside `add_buffers` |
| `input_validation` | `detail::validate_inputs(storage, inputs)`, the real code | Phase 1: count inputs, then per input a type and length check **plus construction of a `where` diagnostic string** |
| `validation_message_replica` | Building the two `where` strings the same way validation does | Part of `input_validation` |
| `executor_setup` | Constructing and destroying `vector<const Buffer*>(3)`, `vector<optional<Buffer>>(3)` and an empty `ExecutionResult` | `bound`, `owned` and `result` (two heap allocations and two frees) |
| `result_buffer_create` | `std::vector<float>(n)`: allocation plus value-initialization. Not "pure malloc". | The result construction in `add_buffers` |
| `result_buffer_release` | Destroying one such vector | Freeing C when `owned` is destroyed at the end of the call |
| `result_create_plus_add` | Create plus loop, without the interpreter or output copy | `add_buffers` |
| `output_materialization` | `outputs.push_back(buffer)` into an empty `vector<Buffer>`, which allocates the outputs array (one element) and allocates plus copies N floats | `result.outputs.push_back(*value)` |
| `data_path_replica` | The executor's allocations and copies **in the same order and with the same lifetimes**: create C, add, wrap in a `Buffer`, copy into a fresh outputs vector, free C. The output lives past the interval. No validation or interpretation. | Everything except validation, bookkeeping and dispatch |
| `pxir_execution_total` | The unmodified `execute_cpu_reference(vp, inputs)`; the `ExecutionResult` is kept and freed after the interval | Canonical full path, timed the same way as M0 |
| `timer_overhead` | An empty interval | Measurement floor |

`fixed_or_dispatch_overhead` is not reported as its own component, because interpretation (op loop, switch, id lookup, binding) was not isolated and has no measured value. It is only bounded indirectly: `pxir_execution_total` is within noise of `input_validation` + `executor_setup` + `data_path_replica` for N ≤ 4,096 (on the batched path), and within noise of `data_path_replica` alone at N ≥ 1M. Any interpretation cost therefore fits inside the residual or noise of this experiment.

## 8. Measurements

Each cell is the **median of three forward-run medians** (51 samples each). The range is across those three runs. "Reverse" and "Clang" are single runs. Faults are median minor page faults per op. K is the batch size.

### N = 1

| Component | K | Median | Run range | Reverse | Clang | Faults/op |
|---|---:|---:|---:|---:|---:|---:|
| `native_add_preallocated` | 1000 | 1.6 ns | 1.5 – 2.2 ns | 1.5 ns | 1.3 ns | 0 |
| `add_loop_preallocated` | 1000 | 1.3 ns | 1.2 – 2.0 ns | 1.2 ns | 1.7 ns | 0 |
| `input_validation` | 1000 | 88 ns | 79 – 90 ns | 87 ns | 80 ns | 0 |
| `validation_message_replica` | 1000 | 61 ns | 58 – 63 ns | 61 ns | 69 ns | 0 |
| `executor_setup` | 1000 | 32 ns | 28 – 32 ns | 31 ns | 33 ns | 0 |
| `result_buffer_create` | 1000 | 8.7 ns | 8.0 – 8.9 ns | 9.3 ns | 9.1 ns | 0 |
| `result_buffer_release` | 1000 | 7.8 ns | 7.7 – 7.8 ns | 8.4 ns | 7.3 ns | 0 |
| `result_create_plus_add` | 1000 | 9.0 ns | 8.9 – 9.1 ns | 13.8 ns | 10.4 ns | 0 |
| `output_materialization` | 1000 | 20 ns | 20 – 21 ns | 29 ns | 23 ns | 0 |
| `data_path_replica` | 1000 | 37 ns | 34 – 37 ns | 36 ns | 45 ns | 0 |
| **`pxir_execution_total`** | 1000 | **150 ns** | 147 – 153 ns | 155 ns | 178 ns | 0 |

### N = 256

| Component | K | Median | Run range | Reverse | Clang | Faults/op |
|---|---:|---:|---:|---:|---:|---:|
| `native_add_preallocated` | 1000 | 24 ns | 22 – 24 ns | 24 ns | 23 ns | 0 |
| `add_loop_preallocated` | 1000 | 25 ns | 22 – 25 ns | 25 ns | 22 ns | 0 |
| `input_validation` | 1000 | 89 ns | 87 – 91 ns | 87 ns | 70 ns | 0 |
| `validation_message_replica` | 1000 | 58 ns | 56 – 61 ns | 55 ns | 66 ns | 0 |
| `executor_setup` | 1000 | 32 ns | 31 – 32 ns | 31 ns | 28 ns | 0 |
| `result_buffer_create` | 1000 | 220 ns † | 216 – 224 ns | **46 ns** | 218 ns | 0.2 |
| `result_buffer_release` | 1000 | 34 ns | 34 – 57 ns | 13 ns | 33 ns | 0 |
| `result_create_plus_add` | 1000 | 240 ns † | 236 – 248 ns | **65 ns** | 250 ns | 0.2 |
| `output_materialization` | 1000 | 88 ns | 81 – 90 ns | 86 ns | 92 ns | 0 |
| `data_path_replica` | 1000 | 113 ns | 112 – 113 ns | 121 ns | 133 ns | 0 |
| **`pxir_execution_total`** | 1000 | **252 ns** | 236 – 258 ns | 280 ns | 257 ns | 0 |

† This is an order artifact. In forward order these K = 1000 allocations run while the heap is still growing, at 0.2 faults per op. In reverse order the heap has already grown and they cost 46 and 65 ns. Treat the reverse values as closer to steady state.

### N = 4,096

| Component | K | Median | Run range | Reverse | Clang | Faults/op |
|---|---:|---:|---:|---:|---:|---:|
| `native_add_preallocated` | 1 | 538 ns | 475 – 576 ns | 727 ns | 556 ns | 0 |
| `add_loop_preallocated` | 1 | 496 ns | 493 – 565 ns | 572 ns | 573 ns | 0 |
| `input_validation` | 1000 | 81 ns | 79 – 90 ns | 93 ns | 81 ns | 0 |
| `validation_message_replica` | 1000 | 61 ns | 58 – 63 ns | 63 ns | 64 ns | 0 |
| `executor_setup` | 1000 | 30 ns | 28 – 32 ns | 32 ns | 31 ns | 0 |
| `result_buffer_create` | 1 | 132 ns | 120 – 135 ns | 136 ns | 134 ns | 0 |
| `result_buffer_release` | 1 | 41 ns | 40 – 42 ns | 43 ns | 44 ns | 0 |
| `result_create_plus_add` | 1 | 745 ns | 676 – 748 ns | 772 ns | 770 ns | 0 |
| `output_materialization` | 1 | 245 ns | 222 – 248 ns | 253 ns | 254 ns | 0 |
| `data_path_replica` | 1 | 976 ns | 870 – 987 ns | 1,125 ns | 998 ns | 0 |
| **`pxir_execution_total`** | 1 | **1,165 ns** | 1,064 – 1,183 ns | 1,451 ns | 1,128 ns | 0 |

K = 1 samples under 1 µs include a 26–30 ns timer floor.

### N = 65,536

| Component | K | Median | Run range | Reverse | Clang | Faults/op |
|---|---:|---:|---:|---:|---:|---:|
| `native_add_preallocated` | 1 | 7.8 µs | 7.2 – 8.0 µs | 8.1 µs | 8.2 µs | 0 |
| `add_loop_preallocated` | 1 | 7.2 µs | 7.2 – 7.8 µs | 8.1 µs | 8.6 µs | 0 |
| `input_validation` | 1000 | 88 ns | 88 – 90 ns | 90 ns | 79 ns | 0 |
| `executor_setup` | 1000 | 31 ns | 30 – 32 ns | 32 ns | 32 ns | 0 |
| `result_buffer_create` | 1 | 5.1 µs | 4.6 – 5.3 µs | 5.3 µs | 5.2 µs | 0 |
| `result_buffer_release` | 1 | 48 ns | 46 – 51 ns | 48 ns | 51 ns | 0 |
| `result_create_plus_add` | 1 | 13.3 µs | 12.1 – 13.7 µs | 13.8 µs | 14.0 µs | 0 |
| `output_materialization` | 1 | 6.3 µs | 5.9 – 7.7 µs | 6.8 µs | 6.7 µs | 0 |
| `data_path_replica` | 1 | 125.0 µs | 120.8 – 172.3 µs | 136.4 µs | 139.2 µs | **96** |
| **`pxir_execution_total`** | 1 | **133.5 µs** | 128.1 – 183.9 µs | 137.6 µs | 140.5 µs | **96** |

### N = 1,048,576

This is the canonical size; the detailed breakdown is in §9.

| Component | K | Median | Run range | Reverse | Clang | Faults/op |
|---|---:|---:|---:|---:|---:|---:|
| `native_add_preallocated` | 1 | 467.6 µs | 435.9 – 470.7 µs | 464.7 µs | 459.6 µs | 0 |
| `add_loop_preallocated` | 1 | 455.9 µs | 450.9 – 456.3 µs | 464.0 µs | 447.4 µs | 0 |
| `input_validation` | 1000 | 88 ns | 87 – 90 ns | 89 ns | 81 ns | 0 |
| `validation_message_replica` | 1000 | 61 ns | 56 – 68 ns | 65 ns | 64 ns | 0 |
| `executor_setup` | 1000 | 31 ns | 29 – 31 ns | 31 ns | 32 ns | 0 |
| `result_buffer_create` | 1 | 145.5 µs | 145.4 – 147.2 µs | 147.4 µs | 148.4 µs | 0 |
| `result_buffer_release` | 1 | 0.19 µs | 0.18 – 0.19 µs | 0.20 µs | 0.16 µs | 0 |
| `result_create_plus_add` | 1 | 599.6 µs | 584.3 – 604.4 µs | 603.9 µs | 606.4 µs | 0 |
| `output_materialization` | 1 | 299.2 µs | 293.8 – 317.5 µs | 310.3 µs | 297.3 µs | 0 |
| `data_path_replica` | 1 | 3,443.9 µs | 3,416.5 – 3,528.7 µs | 3,564.4 µs | 3,540.1 µs | **2016** |
| **`pxir_execution_total`** | 1 | **3,466.3 µs** | 3,371.2 – 3,548.5 µs | 3,370.9 µs | 3,589.9 µs | **2016** |

### N = 4,194,304

| Component | K | Median | Run range | Reverse | Clang | Faults/op |
|---|---:|---:|---:|---:|---:|---:|
| `native_add_preallocated` | 1 | 1,847.6 µs | 1,840.6 – 1,912.6 µs | 1,811.6 µs | 1,865.0 µs | 0 |
| `add_loop_preallocated` | 1 | 1,828.5 µs | 1,825.2 – 1,938.8 µs | 1,781.3 µs | 1,825.4 µs | 0 |
| `input_validation` | 1000 | 97 ns | 79 – 112 ns | 90 ns | 79 ns | 0 |
| `executor_setup` | 1000 | 33 ns | 31 – 41 ns | 33 ns | 31 ns | 0 |
| `result_buffer_create` | 1 | 619.3 µs | 606.7 – 654.5 µs | 619.3 µs | 633.1 µs | 0 |
| `result_buffer_release` | 1 | 0.43 µs | 0.42 – 0.61 µs | 0.44 µs | 0.34 µs | 0 |
| `result_create_plus_add` | 1 | 2,437.5 µs | 2,373.9 – 2,631.5 µs | 2,590.7 µs | 2,536.9 µs | 0 |
| `output_materialization` | 1 | 1,222.7 µs | 1,206.1 – 1,276.3 µs | 1,306.8 µs | 1,238.9 µs | 0 |
| `data_path_replica` | 1 | 15,001.5 µs | 14,463.4 – 16,036.9 µs | 15,810.3 µs | 14,916.3 µs | **8160** |
| **`pxir_execution_total`** | 1 | **15,051.5 µs** | 14,737.9 – 15,223.1 µs | 15,521.3 µs | 14,074.0 µs | **8160** |

### Allocator-configuration experiments (single run each, GCC)

Only the environment changed; the code is identical.

| N | Component | Default | T1: no trim, no mmap | T2: mmap ≥ 128 KiB |
| ---: | --- | ---: | ---: | ---: |
| 65,536 | `result_buffer_create` | 5.1 µs (0 flt) | 5.4 µs (0) | 81.2 µs (65) |
| 65,536 | `pxir_execution_total` | 133.5 µs (96) | **20.9 µs (0)** | 196.7 µs (130) |
| 1,048,576 | `native_add_preallocated` | 467.6 µs | 464.8 µs | 452.0 µs |
| 1,048,576 | `result_buffer_create` | 145.5 µs (0) | 149.9 µs (0) | 1,403.1 µs (1025) |
| 1,048,576 | `output_materialization` | 299.2 µs (0) | 324.7 µs (0) | 1,505.4 µs (1025) |
| 1,048,576 | `data_path_replica` | 3,443.9 µs (2016) | 959.8 µs (0) | 3,703.5 µs (2050) |
| 1,048,576 | `pxir_execution_total` | 3,466.3 µs (2016) | **962.2 µs (0)** | 3,551.7 µs (2050) |
| 4,194,304 | `pxir_execution_total` | 15,051.5 µs (8160) | **3,801.0 µs (0)** | 15,110.0 µs (8194) |

Setting only one of the two tunables also disables glibc's dynamic thresholds, leaving the other at its 128 KiB default. In that case the executor still took 3.3–3.4 ms and `result_buffer_create` became 1.34–1.39 ms. Only T1, which raises both, removes the faults.

**Cost per page fault (derived from T2):** (T2 create − default create) / faults = 1.17 µs (N = 64 Ki), 1.22 µs (N = 1 Mi), 1.24 µs (N = 4 Mi). That is about **1.2 µs per 4 KiB minor fault**, which covers the trap, the kernel's zeroing of the page, and the mapping.

**Mechanism (traced).** Over 10 executor calls at N = 1M, strace shows exactly 30 `brk` calls and no other memory syscalls. Each call grows the heap by 4 MiB (C), grows it by another 4 MiB (the output copy), and shrinks it by 8 MiB when both have been freed ([`strace_executor_n1048576.txt`](data/m1/strace_executor_n1048576.txt)).

## 9. Canonical N = 1,048,576 breakdown

| Measured | Time | Share of total |
| --- | ---: | ---: |
| `pxir_execution_total` | 3,466 µs | 100% |
| `pxir_execution_total` with no page faults (T1) | 962 µs | 28% |
| Difference associated with the 2,016 faults per call | 2,504 µs | 72% |
| Prediction: 2,016 faults × 1.22 µs | 2,460 µs | (98% of that difference) |
| `input_validation` + `executor_setup` | 0.12 µs | < 0.01% |
| **Ratio: total / native** | **7.4×** (default); 2.07× (T1) | |

Inside the fault-free path, all measured in the same T1 run, the isolated components sum to 150 µs (create) + 474 µs (add) + 325 µs (output copy) ≈ 949 µs, against a total of 962 µs. That leaves about 14 µs (1.4%) unattributed. `result_create_plus_add` ≈ `result_buffer_create` + `add_loop_preallocated` at this N (600 vs 601 µs).

## 10. Scaling analysis

| N | Total | Native | Total / native | Faults/call | Dominant cost (measured or associated) |
| ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 150 ns ‡ | 1.6 ns | 94× | 0 | Fixed: validation 88 ns + setup 32 ns + small allocations (batched path) |
| 256 | 252 ns ‡ | 24 ns | 10.5× | 0 | Fixed costs plus two small allocations and a copy (batched path) |
| 4,096 | 1.17 µs | 0.54 µs | 2.2× | 0 | Extra data movement (zero-fill and copy); fixed costs about 10% |
| 65,536 | 133.5 µs | 7.8 µs | **17×** | 96 | Page faults: 96 × 1.17 µs ≈ 112 µs; fault-free total 20.9 µs (2.7×) |
| 1,048,576 | 3,466 µs | 468 µs | 7.4× | 2,016 | Page faults (72%); fault-free total 962 µs (2.07×) |
| 4,194,304 | 15,052 µs | 1,848 µs | 8.1× | 8,160 | Page faults; fault-free total 3,801 µs (2.04×) |

‡ Measured with K = 1000 per interval, with all 1,000 results alive until the interval ends (§4). These values show the scale of fixed costs, not canonical one-call allocator behaviour.

- **Constant costs (measured on all six sizes):** `input_validation` 79–97 ns, `executor_setup` 28–33 ns, timer floor 26–30 ns. They don't vary with N.
- **Costs that scale with N:** the add loop, zero-fill, copy, and fault counts. The fault counts follow **2 × pages(4·N) − 32**: 96 = 2·64 − 32, 2,016 = 2·1,024 − 32, 8,160 = 2·4,096 − 32. That fits both 4·N-byte buffers being returned to the OS each call, apart from 32 pages (128 KiB, glibc's default `M_TOP_PAD`). **Inferred, not traced below N = 1M.**
- **The ratio is not monotonic in N.** It peaks at N = 65,536, where the kernel runs from L2 quickly while every page is re-faulted.

## 11. Data-movement analysis

Per element, counting user-level bytes only. Write-allocate (RFO) reads, hardware prefetch, and cache-line granularity are **not** modelled.

| Path | Operations | Model bytes / element |
| --- | --- | ---: |
| Native (preallocated C) | read A 4, read B 4, write C 4 | 12 |
| Executor: result construction | value-initialize C: write 4 | 4 |
| Executor: add | read A 4, read B 4, write C 4 | 12 |
| Executor: output copy | read C 4, write output 4 | 8 |
| **Executor, minimum** | | **24 (2.0× native)** |
| Executor, default glibc: kernel zeroing of re-faulted pages | 2 buffers × 4 B | +8, plus about 2 × N/1024 page-fault traps |

- **Measured:** every warm streaming component (native, add, create, copy) at N ≥ 1M runs at **26–29 GB/s** of model bytes. The fault-free executor (T1) at N = 1M moves 25.2 MB in 962 µs, which is **26.2 GB/s**, the same rate. Its measured 2.07× ratio to native matches the model's 24/12 = 2.0×.
- **Inferred:** with faults removed, the executor is about 2× native because it moves about 2× the bytes, not because the IR adds per-element work. With default glibc, each call additionally faults and zeroes about 8 MiB of fresh pages, which accounts for most of the remaining gap (§9).
- **Not known:** actual DRAM or L3 traffic, since no hardware counters were available in the container; how much the 12 B/element native model is inflated by RFO; whether huge pages would change the per-fault cost (THP is `madvise` and Lume doesn't madvise).

## 12. Interpretation: the native vs Lume gap at N = 1M

The measured gap is 3,466 − 468 ≈ 3,000 µs:

| Part of the gap | Estimate | Basis |
| --- | ---: | --- |
| Re-faulting heap pages the allocator returned to the OS | ≈ 2,500 µs (84%) | Measured: T1 removes it; 2,016 faults × 1.22 µs predicts it; strace shows the per-call brk grow/shrink |
| Extra warm data movement (zero-fill of C and output copy) | ≈ 445–500 µs (15–17%) | Isolated `result_buffer_create` + `output_materialization`; T1 total minus native |
| Validation and setup (measured); interpretation and dispatch (not measured) | ≈ 0.12 µs measured (< 0.01%), plus an unmeasured interpretation cost | Measured: `input_validation` + `executor_setup`. Interpretation is only bounded indirectly by total − replica, which is within noise. |
| Unexplained | ≈ 0–50 µs, within run-to-run noise (±80 µs) | — |

**Where the gap comes from.** The measured parts of the IR abstraction (validation and bookkeeping) are not a meaningful part of the large-N gap on this machine. Interpretation and dispatch were not measured, but the replica bound leaves no room for them to be meaningful either. The gap comes from how the executor materializes values:
- it allocates a fresh result;
- it copies that result into a second fresh buffer;
- it frees the result inside the call, while the caller frees the output afterwards.

With glibc's default dynamic thresholds, those two adjacent 4 MiB frees cross the trim threshold, so the memory is handed back to the OS on every call and faulted in again on the next one.

**Which values may be added together:**
- **Can be added (approximately, verified):**
  - `result_buffer_create` + `add_loop_preallocated` ≈ `result_create_plus_add` at N ≥ 1M (within 1%), but not at N = 4,096 (628 vs 745 ns).
  - `input_validation` + `executor_setup` + `data_path_replica` ≈ `pxir_execution_total` for N ≤ 4,096 (93–105%).
  - Under T1, `result_create_plus_add` + `output_materialization` ≈ `data_path_replica` (960 vs 960 µs).
- **Must not be added:** isolated `result_create_plus_add` + `output_materialization` (≈ 900 µs) does **not** give the default-allocator executor cost (3,466 µs). The isolated components don't reproduce the executor's buffer lifetimes, which is what causes the trim and re-fault behaviour. `data_path_replica` does reproduce them, and it matches the total within 1% at N ≥ 1M.

## 13. Auto-vectorization

- **Observed:** GCC 13.3 at `-O3` vectorizes the executor's f32 and i32 add loops (`cpu_reference.cpp:45`, `:52`) with 16-byte SSE vectors (`addps`/`paddd`). The loops are versioned at runtime for possible aliasing, and the tail is handled with 8-byte and scalar (`addss`) code. The native oracle loop and the benchmark loops are vectorized the same way. Clang 18 reports width 4 with interleave 2. No AVX is used, because the build has no `-march`, even though the CPU supports AVX2 and AVX-512.
- **Comparison with vectorization disabled** (one run, `-DCMAKE_CXX_FLAGS=-fno-tree-vectorize`; the executor object then contains only `addss`, meaning scalar adds):
  - At N = 256–65,536, which fits in cache, the add loops are 2.9–5.4× slower.
  - At N = 1M the native loop goes from 436 to 734 µs, but executor total stays essentially the same (3.55 vs 3.47 ms), because faults dominate it.
- **Decision:** the canonical build keeps default auto-vectorization. The "scalar" reference executor is scalar in its source, not in its machine code. Both paths get the same treatment.

## 14. Threats to validity

- **Shared virtualized hardware:** a Docker container on a shared VM. Noisy neighbours, the hypervisor's page-table cost (which affects the per-fault cost), and a masked CPU model.
- **Frequency scaling:** no governor is visible, and the frequency can't be pinned from inside the container.
- **Cache state:** the reported L3 is 260 MiB, so the N ≤ 4M working sets fit within it, and the observed rates are consistent with cache-resident execution. Actual cache and DRAM traffic were not measured. A machine with a smaller L3 could show different large-N rates. Preallocated components see a C that is warm from the previous sample.
- **Allocator state and implementation:** this is the dominant effect, and it is **specific to glibc 2.39's dynamic mmap/trim thresholds**. Other allocators (jemalloc, mimalloc, macOS libmalloc, the Windows heap), other glibc versions, or a long-running process with a different heap history could remove or change the fault behaviour entirely. At N = 256 (K = 1000), allocation timings depend on component order through heap growth (†, §8).
- **Batched small-N lifetimes:** for N < 4,096 (K = 1000), produced results, including the 1,000 `ExecutionResult`s of `pxir_execution_total`, stay alive until the interval ends. Output-buffer lifetimes and allocator state therefore differ from one-at-a-time execution. Small-N allocation costs and proportions (for example "about 80% of N = 1") describe this batched path, not canonical single-call behaviour. The N ≥ 4,096 results, including the page-fault finding, use K = 1 and are not affected.
- **OS scheduling:** the max values (for example 0.57 ms against a 0.47 ms median for native at 1M) show preemption. Medians over 51 samples, and medians of three runs, reduce but don't remove this.
- **Compiler version and flags:** GCC 13.3 `-O3` without `-march`. Clang 18.1 results agree with GCC to within about 20% (largest difference: N = 1 total, 178 vs 150 ns). Other compilers are unmeasured.
- **Timing overhead:** the steady_clock interval costs 26–30 ns. That is negligible for batched components, but it inflates K = 1 results under about 1 µs by up to about 5%.
- **Auto-vectorization:** present on both sides, and it matters at cache-resident N (§13).
- **Benchmark ordering:** the fixed forward order was cross-checked with one reverse run. Large-N results agree within about 10%; some small-N allocation results differ by up to 5×.
- **Memory bandwidth:** not measured directly, and there were no hardware performance counters.
- **Equivalence of replicas:** `executor_setup`, `validation_message_replica` and `data_path_replica` are hand-written equivalents of executor code. They could drift from it in the future. `input_validation` avoids that by calling the real function.
- **One machine:** every number here is one environment. The page-fault finding should be re-measured on other allocators and operating systems before it is generalized.

## 15. Conclusions

**MEASURED**

- The unmodified executor at N = 1M takes 3.47 ms (median of three runs), against 0.47 ms for the native preallocated loop. At that size it incurs 2,016 minor page faults per call; the native loop incurs none.
- The executor-form add loop costs the same as the native loop at every N ≥ 256.
- `input_validation` costs 79–97 ns and `executor_setup` 28–33 ns, independent of N. Building the diagnostic strings eagerly, as validation does, costs about 61 ns in isolation.
- A replica of the executor's allocation, copy and free sequence, without validation or interpretation, matches the total within 1% at N ≥ 1M, and within 7% at N = 65,536.
- Keeping glibc from trimming and from using mmap removes every fault and cuts the executor to 0.96 ms (N = 1M) and 3.80 ms (N = 4M), about 2.0–2.1× native. Forcing mmap costs about 1.2 µs per page fault.
- strace shows the heap grow by +4 MiB, +4 MiB and shrink by −8 MiB on every executor call at N = 1M.
- Output checksums are identical across GCC, Clang, the no-vectorize build and both allocator configurations. Every timed result matched the oracle exactly.

**INFERRED**

- About 72% of executor time at N = 1M comes from re-faulting pages that glibc returns to the OS between calls. It is triggered by the executor's two same-sized buffers (result and output copy) being freed together at the top of the heap, where they cross glibc's dynamic trim threshold.
- About 26% is the extra data movement (zero-fill and copy), which doubles the model bytes relative to native.
- The IR abstraction's measured overheads (validation and setup) matter only at small N: on the batched N = 1 path they are about 80% of the 150 ns per-call time. Interpretation and dispatch are inside the residual and were not measured separately.

**NOT YET KNOWN**

- Whether the fault behaviour appears with other allocators or operating systems, or in the Windows and macOS CI environments.
- Actual cache and DRAM traffic, including RFO.
- Interpretation cost (op loop, switch, id lookup) as a separate number. It is only bounded, by being within noise at N = 1.
- The source of the ~14 µs (1.4%) left unattributed in the fault-free path at N = 1M, and the ~1.2 ms (8%) difference between prediction and measurement at N = 4M.

### Candidate optimizations (documented, NOT IMPLEMENTED)

| Candidate | Affected component | Status |
| --- | --- | --- |
| Move the result into `outputs` instead of copying it, when it is the value's last use | `output_materialization`, and possibly the allocator trim pattern | NOT IMPLEMENTED |
| Caller-owned output buffers (write C directly into the caller's memory) | `result_buffer_create`, `output_materialization`, faults | NOT IMPLEMENTED |
| Skip value-initialization of the result, since the add overwrites every element | `result_buffer_create` | NOT IMPLEMENTED |
| Build validation diagnostic strings only on failure | `input_validation` (about 61 of 88 ns) | NOT IMPLEMENTED |
| Reuse the `bound`/`owned` tables across calls | `executor_setup` | NOT IMPLEMENTED |

## 16. Next experiment

Recommended M2 experiment: **remove the output copy by moving the owned result into `outputs` when an output is the last use of that value, and re-measure with this same harness, under both the default and T1 allocator configurations.**

It targets the largest measured and associated costs together: the 8 bytes per element of copy traffic, and (by hypothesis, which M2 must test) the trim and re-fault pattern, since only one large buffer would remain per call. It doesn't change the public API or the execution semantics, it keeps the existing correctness oracle, and it can be falsified cleanly. If the faults remain after the change, the trim hypothesis is wrong.

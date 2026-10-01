# Lume M3: Single-Write Result Construction

> **Archival status: negative research result. The M3 runtime is NOT part of canonical Lume.**
>
> - M3 was executed in experimental commit `2888c0af5a3ced1357cc8383765a27001f13eeba` (branch `pxir/m3-single-write-result-v01`).
> - The implementation built add results with standard C++20 `reserve(n)` + `emplace_back`.
> - The experiment passed every correctness and sanitizer gate: oracle checks, the mutation test, GCC ASan+UBSan, and GCC/Clang `-Werror` builds.
> - It was rejected for canonical integration because of a measured performance regression against M2.
> - Canonical `main` deliberately keeps the M2 result-construction path (`std::vector<T>(n)` + indexed writes).
> - The M3 runtime code, its tests and its benchmark components exist only in the experimental commit (see §0).
> - Present-tense descriptions below ("the M3 executor", "`add_buffers` builds…") refer to that experimental tree.

## 0. Experimental implementation reference

| Item | Value |
| --- | --- |
| Experimental commit | `2888c0af5a3ced1357cc8383765a27001f13eeba` |
| Base (canonical M2 `main`) | `594a223b97ef8b1eea668f0bc142e1e08a33f39c` |
| Experimental branch | `pxir/m3-single-write-result-v01` |
| Files that exist only there | the `add_buffers` change in `src/runtime/cpu_reference.cpp`, `tests/execution/single_write_test.cpp` (+ `tests/CMakeLists.txt` entry), and the M3 components and 12 B/element model in `benchmarks/executor_breakdown.cpp` |

The runtime implementation is **intentionally absent** from canonical `main`, because M3 was rejected. Only this report and its raw data ([`docs/data/m3/`](data/m3/)) are carried into the canonical history.

To reproduce the experiment, check out `2888c0a` for B and `594a223` for A, then follow §8.

M3 was a controlled experiment with exactly one runtime change. In the experimental tree, `add_buffers` built its result with `reserve(n)` followed by `emplace_back` for each element. Each element is constructed once, with its final value, instead of `std::vector<T>(n)` (value-initialization, a zero-fill) followed by indexed writes. The change was measured with the M1/M2 method against canonical M2, built from `main` on the same machine.

**Result:** the redundant N-sized zero-fill is gone, and the minimum user-level model drops from 16 to 12 B/element. However, neither GCC 13.3 nor Clang 18.1 vectorizes the `emplace_back` loop, and the loop costs about 1 ns per element even without SIMD. M3 is **slower than M2 at every N ≥ 256**: 1.6–1.7× at N ≥ 1M with GCC and 1.2× with Clang. **Recommendation: RESEARCH RESULT ONLY — DO NOT MERGE** (§19).

Every number is from one machine (glibc 2.39, GCC 13.3.0, Clang 18.1.3, libstdc++ 13). None of it is a general performance claim.

## 1. Research question

Can Lume construct an add result exactly once, without first zero-initializing its N elements, while remaining standard-conforming C++20 and keeping all semantics?

Secondary questions:

1. Does payload traffic fall from 16 to 12 B/element?
2. What happens to total time?
3. Is auto-vectorization preserved?
4. If it is lost, does that outweigh the saved write?
5. What happens to page faults under default, T1 and T2?
6. What is the effect at small N?
7. Does i32 wrapping stay correct?
8. Is `std::vector<T>` a suitable result representation for single-write construction?

## 2. M2 baseline

Canonical M2 is `main` at `594a223b97ef8b1eea668f0bc142e1e08a33f39c`. At N = 1,048,576, M2 measured native 0.45 ms and Lume 0.60 ms (1.31× native), with 0 faults. The model was 16 B/element: 4 for value-initialization, 12 for the add, and 0 N-scaled bytes for the output move. `result_buffer_create` measured about 149 µs, close to the M2 − native gap.

The baseline used in this document is the A build, re-measured on the same machine in the same session (§8).

## 3. Hypotheses (stated before measuring)

| Id | Hypothesis | Outcome |
| --- | --- | --- |
| H1 | `reserve` + `emplace_back` removes the N-sized value-initialization | **Supported.** `result_buffer_reserve` costs 44–51 ns at every N ≥ 4,096, against 160 µs for `result_buffer_create` at N = 1M. |
| H2 | The model falls from 16 to 12 B/element | **Supported by construction.** It is not visible in time, because the loop is no longer memory-bound (§13). |
| H3 | Under T1, M3/M2 ≈ 12/16 = 0.75 | **Falsified.** M3/M2 = 1.73 at N = 1M and 4M (T1, 3 pairs). |
| H4 | `emplace_back` may inhibit auto-vectorization | **Supported.** Neither GCC nor Clang vectorizes either M3 add loop; M2's loops are vectorized by both (§14). |
| H5 | Under forced mmap (T2), demand-zero page faults remain about one per result page | **Supported.** 1,025 faults per call at N = 1M in both M2 and M3; the syscall pattern is identical. |
| "149 µs" | M3 removes about 149 µs at N = 1M (M3 ≈ native) | **Falsified (Outcome C).** M3 is 451 µs *slower* than M2 at N = 1M (GCC, default allocator). |

## 4. C++ safety constraint

Performance obtained through undefined behavior is not valid Lume evidence. M3 therefore does **not** write through `data()` beyond `size()`, touch implementation internals (`_M_finish` or similar), placement-new into reserved storage, or use a custom allocator or raw buffer. The only standard operations used are `reserve` and `emplace_back`, each of which constructs one element at `end()` and grows `size()` by one.

## 5. Implementation

The only runtime change, in experimental commit `2888c0a` (not on canonical `main`), is in `add_buffers`, in `src/runtime/cpu_reference.cpp`:

```cpp
// f32 (i32 is identical but appends wrapping_add(a[i], b[i]))
std::vector<float> c;
c.reserve(a->size());
for (std::size_t i = 0; i < a->size(); ++i) c.emplace_back((*a)[i] + (*b)[i]);
return Buffer(std::move(c));
```

Unchanged: `Buffer`, the IR, the verifier, typed ids, validation, the last-use table, the M2 output move and unbind, the public API, and compiler flags.

- **Exception safety:** `reserve(n)` may throw `std::length_error` or `std::bad_alloc` before any element exists. After a successful `reserve(n)`, the n appends never reallocate (`size()` stays ≤ `capacity()`). `float` and `int32_t` construction cannot throw. If anything throws, the local vector is destroyed and the exception propagates out of `execute_cpu_reference` exactly as before M3: no partial result escapes, and the error API is unchanged.

## 6. Correctness invariants

| Invariant | Why it holds |
| --- | --- |
| `c.size() == lhs.size() == rhs.size()` on success | The sizes are checked equal first, and the loop appends exactly `a->size()` elements |
| Each element is constructed exactly once, before it can be observed | `emplace_back` constructs element i with its final value; nothing reads `c` until the loop ends |
| No write outside `[0, size())` | Only `emplace_back` writes, and it writes at `end()` |
| N = 0 is not reachable | The verifier rejects zero-length types (`zero_length`). That rule is kept, not extended. |
| f32 exact semantics | Same `a + b` expression, same flags, no fast-math |
| i32 wraps modulo 2^32 | Still `wrapping_add`, computed through `uint32_t` |
| M2 output ownership | `last_use`, the move, `slot.reset()` and `bound[id] = nullptr` are untouched |

## 7. Benchmark methodology

This uses the M1/M2 harness and statistics unchanged (see [`m1-executor-cost-breakdown.md`](m1-executor-cost-breakdown.md) §4, including the small-N batching caveat).

- **Historical names keep their meaning:** `result_buffer_create`, `result_create_plus_add` and `data_path_move_replica` remain the value-initialized (zero-fill) path, as controls.
- **New M3 components** (in the experimental tree's `benchmarks/executor_breakdown.cpp` only):
  - `result_buffer_reserve`: `std::vector<float> c; c.reserve(n);`. Capacity only; 0 payload B/element (allocator work still exists).
  - `add_construct_reserved`: the `emplace_back` loop into a vector with size 0 and capacity ≥ N. The state is restored untimed with `clear()` + `reserve(n)`. Model: 12 B/element.
  - `result_reserve_plus_add`: reserve plus the `emplace_back` loop from an empty vector. This is M3's `add_buffers` data path. Model: 12 B/element.
  - `data_path_single_write_replica`: reserve, construct once, wrap in an owned slot, move into a fresh outputs vector, with the output alive past the interval. Model: 12 B/element.
- **Tree-dependent value:** `pxir_execution_total` measures the executor of the tree it is built in. Its model was 12 B/element in the experimental M3 tree; canonical `main` keeps M2's 16.
- **Benchmark helpers compile like the executor:** the indexed helper (`executor_style_add`) is vectorized with 16-byte vectors, and the emplace helper (`executor_style_emplace_add`) is not (GCC report).

## 8. A/B methodology

- **A** is canonical M2 `main` (`594a223`), built in a separate git worktree. **B** is the experimental M3 tree (commit `2888c0a`, branch `pxir/m3-single-write-result-v01`). Both use GCC 13.3.0, CMake Release (`-O3 -DNDEBUG`), `-DLUME_WARNINGS_AS_ERRORS=ON`, the same machine, and one session.
- Runs strictly alternate A and B:
  - default allocator: 6 pairs, N ∈ {1, 256, 4,096, 65,536, 1M, 4M};
  - T1: 3 pairs;
  - T2: 1 pair;
  - reverse component order: 1 pair;
  - dedicated small-N: 10 pairs at N ∈ {1, 256}, 201 iterations;
  - `lume_bench_vector_add`: 3 pairs;
  - Clang 18.1.3 A/B: 2 pairs, both trees built with Clang;
  - no-vectorize control: 2 pairs, both trees built with `-fno-tree-vectorize`.
- The environment matches M1 §5: 4-vCPU Xeon at 2.10 GHz in a Docker VM, reported L3 260 MiB, Linux 6.18, 4 KiB pages, glibc 2.39.

Raw outputs are in [`docs/data/m3/`](data/m3/) (`A_*` = M2, `B_*` = M3), together with the strace traces and [`vectorization_evidence.txt`](data/m3/vectorization_evidence.txt).

## 9. Default allocator results

`pxir_execution_total`, GCC, medians over 6 alternating pairs:

| N | Native (B runs) | M2 total (A) | M3 total (B) | M3/M2 | Per pair | M3/native | Faults M2 / M3 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 ‡ | 1.4 ns | 170 ns | 177 ns | 1.04 | 0.97 – 1.11 | — | 0 / 0 |
| 256 ‡ | 26.1 ns | 281 ns | 470 ns | 1.67 | 1.64 – 1.72 | 18.0× | 0 / 0 |
| 4,096 | 584 ns | 872 ns | 3,924 ns | 4.50 | 3.08 – 4.97 | 6.7× | 0 / 0 |
| 65,536 | 8.39 µs | 14.4 µs | 60.1 µs | 4.16 | 3.86 – 4.19 | 7.2× | 0 / 0 |
| 1,048,576 | 477.9 µs | 631.6 µs | 1,083.2 µs | 1.72 | 1.64 – 1.77 | 2.27× | 0 / 0 |
| 4,194,304 | 1,944.6 µs | 2,636.8 µs | 4,295.2 µs | 1.63 | 1.59 – 1.71 | 2.21× | 0 / 0 |

‡ These are measured on the batched K = 1000 path; see §15 for the dedicated small-N A/B.

- **Reverse-order pair** (M2 / M3): N = 65,536: 14.4 / 59.9 µs; N = 1M: 641 / 1,097 µs; N = 4M: 2,630 / 5,136 µs.
- **`lume_bench_vector_add`** (3 pairs): M2 0.63–0.67 ms and M3 1.09–1.10 ms, with the same checksum.
- **Clang A/B** (2 pairs, both built with Clang), M3/M2: 1.35 (N = 1), 1.67 (256), 3.63 (4,096), 3.19 (65,536), 1.21 (1M), 1.24 (4M).

### Canonical N = 1,048,576 (GCC, default allocator; B-tree components, medians of 6 runs)

| Component | Path | Median | Faults/op |
|---|---|---:|---:|
| `native_add_preallocated` | — | 477.9 µs | 0 |
| `result_buffer_create` | M2 control | 159.8 µs | 0 |
| `add_loop_preallocated` | M2 control (indexed, vectorized) | 470.9 µs | 0 |
| `result_create_plus_add` | M2 control | 637.5 µs | 0 |
| `result_buffer_reserve` | M3 | **49.5 ns** | 0 |
| `add_construct_reserved` | M3 (emplace loop, not vectorized) | **1,081.9 µs** | 0 |
| `result_reserve_plus_add` | M3 | 1,078.5 µs | 0 |
| `data_path_move_replica` | M2 control | 635.2 µs | 0 |
| `data_path_single_write_replica` | M3 | 1,084.6 µs | 0 |
| `pxir_execution_total` (A = M2) | M2 | 631.6 µs | 0 |
| `pxir_execution_total` (B = M3) | M3 | 1,083.2 µs | 0 |

The zero-fill disappeared (160 µs → 50 ns), but the write loop went from 471 µs (indexed, vectorized) to 1,082 µs (emplace, scalar). The net effect is +451 µs.

## 10. T1 results (glibc never trims or mmaps)

| N | Native | M2 | M3 | M3/M2 | Per pair |
|---:|---:|---:|---:|---:|---:|
| 65,536 | 8.48 µs | 14.9 µs | 61.2 µs | 4.11 | 2.94 – 4.33 |
| 1,048,576 | 475.4 µs | 631.2 µs | 1,091.6 µs | 1.73 | 1.71 – 1.75 |
| 4,194,304 | 1,948.6 µs | 2,566.6 µs | 4,442.5 µs | 1.73 | 1.63 – 1.81 |

T1 changes nothing here, because neither build faults under default glibc anymore (since M2). The byte-model prediction of 0.75 is falsified: M3/M2 is 1.73.

## 11. T2 results (`glibc.malloc.mmap_threshold=131072`, 1 pair)

| N | M2 total (faults) | M3 total (faults) | M2 `result_buffer_create` | M3 `result_buffer_reserve` |
|---:|---:|---:|---:|---:|
| 65,536 | 99.6 µs (65) | 123.5 µs (65) | 84.5 µs (65) | 3.2 µs (1) |
| 1,048,576 | 1,957 µs (1,025) | 2,187 µs (1,025) | 1,514 µs (1,025) | 3.4 µs (1) |
| 4,194,304 | 8,424 µs (4,097) | 9,061 µs (4,097) | 5,951 µs (4,097) | 3.5 µs (1) |

- **Measured:** with fresh mmap'd memory, M3's `reserve` faults once (the page holding the allocation header) instead of 1,025 times. The faults do not disappear, though: they move into the `emplace_back` loop, and the executor still takes 1,025 faults per call, exactly like M2. The kernel's demand-zero work (a fault plus zeroing per 4 KiB page on first write) is unaffected by removing the user-level zero-fill. Only the user-level write is gone.

## 12. Page-fault analysis

| N | Default M2 / M3 | T1 M2 / M3 | T2 M2 / M3 |
|---:|---:|---:|---:|
| 65,536 | 0 / 0 | 0 / 0 | 65 / 65 |
| 1,048,576 | 0 / 0 | 0 / 0 | 1,025 / 1,025 |
| 4,194,304 | 0 / 0 | 0 / 0 | 4,097 / 4,097 |

**strace** (probe [`docs/data/m2/strace_probe.cpp`](data/m2/strace_probe.cpp), 10 calls at N = 1M; the traces are in `docs/data/m3/strace_*`):

| Build | Default glibc | T2 |
|---|---|---|
| M2 (A) | 1 `brk`, then no memory syscalls | 10 × (`mmap` 4,198,400 B + `munmap`) |
| M3 (B) | 1 `brk`, then no memory syscalls | 10 × (`mmap` 4,198,400 B + `munmap`) |

Removing value-initialization does not change the allocator's system-call behavior, because allocation sizes and lifetimes are unchanged.

## 13. Data-movement model

| Stage | M2 | M3 |
| --- | ---: | ---: |
| Result construction (value-initialize) | 4 | 0 (capacity only) |
| Add: read A, read B, write C | 12 | 12 |
| Output (single, final use) | 0 N-scaled | 0 N-scaled |
| **Minimum user-level B/element** | **16** | **12** (equal to native) |

Not modelled: RFO, cache-line granularity, allocator metadata, the per-element bookkeeping of `emplace_back`, and the kernel's demand-zero page writes when pages are fresh (T2).

- **Measured:** time per element for the M3 construction loop is nearly flat across working-set sizes: 0.88 (N = 256, batched), 0.90 (65,536), 1.03 (1M) and 1.05 (4M) ns/element. The indexed M2 loop measures 0.12–0.18 ns/element when cache-resident and 0.45 at N ≥ 1M.
- **Inferred:** the M3 loop is limited by instructions per element, not by memory traffic. Removing 4 B/element of traffic therefore cannot show up in the time. The model reduction is real, but it does not bind.

## 14. Auto-vectorization analysis

Full reports are in [`vectorization_evidence.txt`](data/m3/vectorization_evidence.txt). Its B line numbers refer to `src/runtime/cpu_reference.cpp` at `2888c0a`. Flags: `-O3 -DNDEBUG`, no `-march` (SSE2 baseline).

| Loop | GCC 13.3 | Clang 18.1 |
|---|---|---|
| M2 f32 (indexed) | Vectorized, 16-byte vectors (`addps`); loop versioned for aliasing | Vectorized, width 4, interleave 2 |
| M2 i32 (indexed) | Vectorized, 16-byte vectors (`paddd`) | Vectorized, width 4, interleave 2 |
| M3 f32 (emplace) | **Not vectorized**: "vectorized 0 loops in function"; "splitting region at control altering definition … operator new"; "Analysis failed with vector mode VOID" | **Not vectorized**: "could not determine number of loop iterations"; "value that could not be identified as reduction is used outside the loop" |
| M3 i32 (emplace) | **Not vectorized** (same messages) | **Not vectorized** (same messages) |

In the M3 objects there is no `addps` and no `paddd`, only one scalar `addss`. The GCC inner loop, per element:
- reloads A's data pointer and recomputes A's size;
- does one scalar `addss`;
- compares the end pointer with the capacity, branching to the reallocation path (`operator new`);
- stores the element and bumps the end pointer.

Inferred for these toolchains only: the reallocation path that `emplace_back` keeps in the loop (the compiler cannot prove it is never taken) and the end pointer, which is live after the loop, are what block vectorization. Clang's diagnostics name exactly those two conditions.

**No-vectorize control** (both trees built with `-fno-tree-vectorize`, 2 pairs):

| N | M2 total | M3 total | M3/M2 | M2 indexed scalar loop | M3 emplace loop |
|---:|---:|---:|---:|---:|---:|
| 65,536 | 32.9 µs | 58.3 µs | 1.77 | 26.1 µs | 58.6 µs |
| 1,048,576 | 658.3 µs | 1,089.4 µs | 1.65 | 505.9 µs | 1,097.4 µs |
| 4,194,304 | 2,888.8 µs | 4,352.2 µs | 1.51 | 2,252.9 µs | 4,482.3 µs |

- **Measured:** even when both loops are scalar, the emplace loop takes about 2× as long as the indexed loop, and M3 stays 1.5–1.8× slower than M2. M3's time is the same with and without `-fno-tree-vectorize` (1,089 vs 1,083 µs at 1M), consistent with its loop already being scalar.
- **Inferred:** lost SIMD accounts for only part of the regression. Most of it is the per-element work of `emplace_back` in libstdc++ as GCC compiles it.

## 15. Small-N results

Dedicated small-N A/B, 10 pairs of 201 iterations, batched K = 1000 path:

| N | M2 | M3 | Paired difference (median, range) | M3 slower in |
|---:|---:|---:|---:|---:|
| 1 | 172.7 ns | 176.6 ns | +4.3 ns (−82.0 to +25.4) | 7 / 10 |
| 256 | 279.0 ns | 468.4 ns | **+189.8 ns** (+179.3 to +215.2) | **10 / 10** |

At N = 256 the components show the cause directly: `add_construct_reserved` 225 ns vs `add_loop_preallocated` 46 ns, and `result_buffer_reserve` 31 ns vs `result_buffer_create` 48 ns. At N = 1 the difference is near the noise floor. At N = 256 the regression is clear and consistent.

## 16. Correctness

- **Oracle:** every benchmark output in `docs/data/m3/` is free of `MISMATCH`. Checksums are identical between M2-GCC, M3-GCC and M3-Clang at every N (N = 1M: `0xf09ed3431c02ceea`, the same as M0, M1 and M2).
- **New tests** (`tests/execution/single_write_test.cpp`, suite `lume_single_write_test`, in the experimental commit only), each checking result length and bit patterns against the oracle:
  - **A/B:** f32 at N ∈ {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 4099}.
  - **C:** ±0, ±∞, NaN, max, denormals and a ties-to-even case at the start, middle and tail of a 17-element vector; −0 + −0 keeps its sign.
  - **D:** i32 `MAX+1`, `MIN+(−1)`, `MAX+MAX`, `MIN+MIN`, `0+MIN`, `MAX+MIN`, and random data at N ∈ {1, 5, 9, 17, 4099}.
  - **E:** the chained add `D = (A + B) + A`.
  - **F:** the M2 output rules on single-write results (copy before a later use, then move; inputs unchanged).
- **Mutation test** (temporary, not committed): both loops changed to `i + 1 < a->size()`, omitting the last element. It was killed by 5 tests: `lume_single_write_test` (length and oracle), `lume_execution_test`, `lume_output_move_test`, `lume_bench_vector_add_smoke` (no `correctness=exact`) and `lume_bench_executor_breakdown_smoke` (`MISMATCH`). All 16 tests pass after the restore.

## 17. Threats to validity

- **Everything from M1 §14 and M2 §16 still applies:** a shared VM, no frequency control, cache residency unverified by hardware counters, one machine, allocator dependence, the timer floor, and small-N batching.
- **Standard-library and compiler dependence:** this result is specific to libstdc++ 13's `emplace_back` as compiled by GCC 13.3 and Clang 18.1 at `-O3` without `-march`. A different standard library, compiler version or target ISA could vectorize the loop, or lower its per-element bookkeeping. The size of the regression already differs between compilers: 1.72× (GCC) vs 1.21× (Clang) at N = 1M.
- **Code layout:** A and B are different binaries. The B benchmark runs four extra components before `pxir_execution_total` in forward order, but the reverse-order pair agrees.
- **Replica fidelity:** `data_path_single_write_replica` matches the M3 total within 2% at N ≥ 65,536.

## 18. Findings

**MEASURED**

- **Zero-fill removed:** the user-level zero-fill is gone (`result_buffer_reserve` 44–51 ns vs `result_buffer_create` 160 µs at 1M).
- **Not vectorized:** GCC 13.3 and Clang 18.1 both fail to vectorize the M3 f32 and i32 loops, while both vectorize the M2 loops.
- **Slower:** M3 is slower than M2 at every N ≥ 256.
  - GCC default: 1.67× (256), 4.50× (4,096), 4.16× (65,536), 1.72× (1M), 1.63× (4M).
  - T1: 1.73× at 1M and 4M.
  - Clang: 1.21–1.24× at N ≥ 1M.
- **Scalar vs scalar:** with vectorization disabled in both builds, M3 is still 1.5–1.8× slower, because the emplace loop costs about 2× the indexed scalar loop.
- **Page faults:** unchanged. 0 / 0 under default and T1, and identical (1,025 per call at 1M) under T2. The allocator syscalls are identical too.
- **Small N:** +4.3 ns at N = 1 (noise-level) and +190 ns at N = 256 (10 of 10 pairs).
- **Correctness:** all correctness gates pass; checksums are identical.

**INFERRED**

- The M3 loop is instruction-bound at about 1 ns/element (flat across N), so the saved 4 B/element cannot appear in the time.
- In these toolchains (libstdc++ 13 with GCC 13.3 and Clang 18.1, `-O3`, no `-march`), the reallocation branch and the live end pointer inside `emplace_back` block vectorization (Clang's diagnostics name both). This is not a claim about every C++ implementation.
- Most of the regression comes from `emplace_back`'s per-element work, not only from lost SIMD.

**FALSIFIED**

- "M3/M2 ≈ 0.75 under T1" (the byte model): measured 1.73.
- "M3 removes about 149 µs at N = 1M, approaching native": M3 is 451 µs slower than M2.
- "Removing value-initialization removes page faults under forced mmap": faults are unchanged, because the kernel's demand-zero work moves into the construction loop.

**NOT YET KNOWN**

- Whether other standard libraries, compilers or `-march` targets vectorize or cheapen this loop.
- Actual cache and DRAM traffic.
- Whether a representation that exposes uninitialized storage in a standard-conforming way, such as a result type not based on `std::vector`, keeps the vectorized indexed loop while dropping the zero-fill.

## 19. Merge recommendation

**RESEARCH RESULT ONLY — DO NOT MERGE.**

M3 is correct, standard-conforming C++20, green under ASan+UBSan and portable. But it regresses the canonical executor by 1.6–1.7× at large N and by up to 4.5× at mid N (GCC), and it regresses under Clang too. The payload saving is real in the model, but on this toolchain the cost of `emplace_back`'s per-element construction outweighs it. Replacing M2 with M3 would make the canonical Lume executor slower. M2 should remain canonical. This branch documents the negative result and the limitation it exposes.

## 20. Next experiment

Recommended M4 experiment: **introduce a result representation that owns uninitialized storage in a standard-conforming way.**
- **How:** allocate raw storage with `std::allocator<T>::allocate(n)`, construct each element exactly once with `std::construct_at` through an indexed loop over a pointer the compiler can analyze, and hand it to `Buffer` as an owning type.
- **Measure:** A/B against M2 with this harness, including vectorization reports.
- **Why this one:**
  - M3 showed that the bottleneck is `std::vector`'s append interface, not the removed write.
  - The question is whether a single-write indexed loop keeps M2's vectorization while dropping the 4 B/element zero-fill.
  - It is a `Buffer` representation change, so it needs its own safety analysis (lifetime of partially constructed ranges, exception safety, ownership through moves).
- **Status:** not implemented.

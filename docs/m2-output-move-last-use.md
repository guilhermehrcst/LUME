# PXIR M2: Move Owned Output on Last Use

M2 is a controlled experiment with exactly one runtime change. When an `output` operation is the final use of a value the executor owns, the executor transfers that `Buffer` into `ExecutionResult::outputs` instead of deep-copying it. Every other output still copies. Everything else was measured with the M1 method, against canonical M1 built from `main` on the same machine.

Every number is from one machine and one allocator (glibc 2.39). None of it is a general performance claim about PXIR.

## 1. Research question

What happens if PXIR stops deep-copying an executor-owned result when an output is that result's final use, and instead transfers ownership of the `Buffer` directly into the `ExecutionResult`?

Secondary questions:

1. How much user-level data movement disappears?
2. Does the second large allocation disappear?
3. What happens to the glibc trim/re-fault pattern?
4. What happens under the M1 T1 allocator configuration?
5. What fixed cost does last-use analysis add?
6. Do multiple outputs and later uses stay correct?
7. Does performance change at small and at large N?
8. How strongly does the result depend on the allocator?

## 2. M1 baseline

Canonical M1 is `main` at `7e6ec8aabdfbf4e1b1d791b44a6124c594b0106f`. At N = 1,048,576, M1 measured:
- native preallocated add: about 0.47 ms;
- executor: 3.47 ms with 2,016 minor faults per call;
- executor under T1: 0.96 ms.

The M1 user-level model was 24 B/element, against 12 for native. M1 attributed about 72% of executor time to re-faulting pages that glibc trims after the result and its output copy are freed together (strace: +4 / +4 / −8 MiB per call). The baseline used in this document is the A build, re-measured on the same machine in the same session (§8), not these historical numbers.

## 3. Hypotheses (stated before implementing)

| Id | Hypothesis | Outcome |
| --- | --- | --- |
| H1 | For `C = A + B; output C`, the N-sized output copy can be eliminated. | **Supported.** Moving the output costs 0.07–0.6 µs at N ≥ 65,536 and does not grow with N, while the copy costs 300 µs at N = 1M. strace shows the second heap growth gone. |
| H2 | The minimum user-level model falls from 24 to 16 B/element. | **Supported by construction and by measurement.** Under T1, M2/M1 is 0.60–0.66, against a model ratio of 16/24 = 0.667. |
| H3 | With faults removed (T1), M2 approaches `result_buffer_create` + `add_loop`, not create + add + copy. | **Supported.** At N = 1M under T1, M2 is 592.8 µs, against `result_create_plus_add` 605.0 µs and `data_path_replica` (copy) 917.7 µs. |
| H4 | Removing the second N-sized allocation may change the glibc trim/re-fault behavior. | **The faults disappeared under default glibc on this machine:** 2,016 → 0 at N = 1M, and 8,160 → 0 at N = 4M. strace shows one heap growth on the first call and no memory syscalls after that. This depends on the allocator: with every large block forced through mmap (T2), M2 still incurs 1,025 faults per call. |
| H5 | Last-use analysis adds a fixed cost that may slow tiny workloads. | **Isolated cost measured:** 20–23 ns. **The full-executor change at N = 1 is small and near the noise floor:** +6.3 ns paired median, M2 slower in 8 of 10 pairs, range −12.9 to +26.9 ns. At N = 256 it is −6.2 ns (M2 slower in 4 of 10), so neutral. |

## 4. Implementation design

This is the only runtime change, in `src/runtime/cpu_reference.cpp`:

```cpp
// output op i, value id = op.operands[0]
std::optional<Buffer>& slot = owned[id.index()];
if (slot.has_value() && last_use[id.index()] == OperationId{i}) {
    result.outputs.push_back(std::move(*slot));   // transfer, no N-sized copy
    slot.reset();
    bound[id.index()] = nullptr;                  // unreachable afterwards
} else {
    result.outputs.push_back(*value);             // M1 behavior: copy
}
```

Unchanged: the IR, typed ids, the verifier, `VerifiedProgram`, the public API (`execute_cpu_reference`, `Buffer`, `ExecutionResult`), input validation, zero-initialization of results, `bound` and `owned`, the error codes and messages, and compiler flags. The only other source change is the doc comment in `include/pxir/runtime/cpu_reference.hpp`, which now describes the move rule.

`static_assert(std::is_nothrow_move_constructible_v<Buffer>)` records what the design relies on. `Buffer` wraps a `std::variant` of two `std::vector`s, so its implicit move is `noexcept`. As with moved-from standard-library containers, the source remains valid but its value must be treated as unspecified. PXIR does not depend on that state: the owned slot is reset and the value unbound immediately after the move. The same `noexcept` property lets `std::vector<Buffer>` move existing elements rather than copy them when `outputs` grows.

## 5. Last-use analysis

Before interpretation, the executor builds one table, local to each call:

```cpp
std::vector<OperationId> last_use(s.values.size());          // invalid id = never read
for (i = 0 .. operations.size()) for (operand : operations[i].operands)
    if (operand.is_valid()) last_use[operand.index()] = OperationId{i};
```

It is one pass over the operations. It is correct for M0's straight-line IR because the program is verified: every operand id exists and is defined before it is used, and the operation order is fixed. Both `add` and `output` count as uses. There is no graph algorithm, and nothing is cached in `VerifiedProgram`.

- **Cost:** one heap allocation per call (`values.size() × 4` bytes, 12 bytes for the canonical program) plus the pass itself. Isolated, this costs 20–23 ns (`last_use_analysis`, K = 1000, §14). It has not been optimized.

## 6. Semantic invariants

| Invariant | Why it holds | Evidence |
| --- | --- | --- |
| A caller's input is never moved or modified | Only `owned[v]` is ever moved from, and it is filled only by `add` results, whose storage `add_buffers` allocates separately. Inputs are reached only through `const Buffer*` into the caller's span. An output of an input takes the copy branch because its `owned` slot is empty. | Tests A–G check the caller's buffers after execution (`run()` checks both, F checks the i32 inputs) |
| No move before a later use | A move requires `last_use[v] == i`, so no later operation reads `v` | Tests B, C, G; mutation test (§15) |
| A moved-from value is unreachable | After the move, `slot.reset()` and `bound[v] = nullptr`. Any later lookup would fail closed with an internal error instead of reading the moved-from Buffer, whose contents are unspecified. | Mutation 2 (§15) shows what happens without the reset |
| Output order is IR order | Each output still appends exactly once, in operation order | Test G (mix of moves and copies) |
| Error semantics are unchanged | Validation, the `unbound` check and the kernel checks are untouched; the new branch creates no new error | All M0 and M1 error tests unchanged and green |
| Repeated execution is unaffected | The move touches only per-call state (`owned`, `bound`, `result`), never the `VerifiedProgram` | Test `repeated_execution_is_stable` |

## 7. Benchmark methodology

This uses the M1 harness (`pxir_bench_executor_breakdown`) and M1's statistics: 5 warmup and 51 measured samples per (N, component), the median reported, K = 1000 for size-independent operations and for N < 4096, and K = 1 otherwise. Page faults come from `getrusage`, and every computed result is checked against the oracle outside the timed interval. See [`m1-executor-cost-breakdown.md`](m1-executor-cost-breakdown.md) §4, including its batching caveat for small N.

Changes to the benchmark:

- **Unchanged M1 names and definitions:** `output_materialization` and `data_path_replica` are the output **copy** path. They are kept as the copy baseline, not redefined.
- **New components:**
  - `output_move_materialization`: `outputs.push_back(std::move(*slot)); slot.reset();` into a fresh outputs vector. The N-sized source is created untimed.
  - `data_path_move_replica`: create C, add, wrap it in an owned slot, move it into a fresh outputs vector, with the output alive past the interval. This is the lifetime-faithful M2 data path, with no interpreter.
  - `last_use_analysis`: the executor's table construction.
- **Tree-dependent value:** `pxir_execution_total` measures the executor of the tree it is built in. Its model is now 16 B/element; it was 24 in M1.

## 8. A/B methodology

- **A** is canonical `main` (`7e6ec8a`), built in a separate git worktree. **B** is this branch. Both use GCC 13.3.0, CMake Release (`-O3 -DNDEBUG`), `-DPXIR_WARNINGS_AS_ERRORS=ON`, and each binary runs its own tree's benchmark.
- Runs strictly alternate A, B, A, B on the same machine in the same session:
  - default allocator: 6 pairs, all six N;
  - T1: 3 pairs, N ∈ {65,536, 1M, 4M};
  - T2: 1 pair;
  - reverse component order: 1 pair;
  - small-N: 10 pairs at N ∈ {1, 256} with 201 iterations;
  - canonical `pxir_bench_vector_add`: 3 pairs.
- "M2/M1" is the ratio of the medians across runs. The range is the ratio within each A/B pair.
- A Clang 18.1.3 B run was used as a cross-check.
- The environment matches M1 §5: 4-vCPU Xeon at 2.10 GHz in a Docker VM, L3 reported at 260 MiB, Linux 6.18, 4 KiB pages, glibc 2.39.

Raw outputs are in [`docs/data/m2/`](data/m2/) (`A_*` = M1, `B_*` = M2).

## 9. Default allocator results

`pxir_execution_total` medians over 6 alternating pairs:

| N | Native (B runs) | M1 total (A) | M2 total (B) | M2/M1 | M2/M1 per pair | M2/native | M1 faults | M2 faults |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 ‡ | 1.9 ns | 155 ns | 168 ns | 1.08 | 0.97 – 1.75 | — | 0 | 0 |
| 256 ‡ | 24.3 ns | 257 ns | 260 ns | 1.01 | 0.78 – 1.73 | 10.7× | 0 | 0 |
| 4,096 | 566 ns | 1,116 ns | 829 ns | 0.74 | 0.55 – 0.82 | 1.46× | 0 | 0 |
| 65,536 | 8.07 µs | 127.8 µs | 13.5 µs | 0.105 | 0.062 – 0.113 | 1.67× | 96 | **0** |
| 1,048,576 | 454.0 µs | 3,286.1 µs | 596.6 µs | 0.182 | 0.169 – 0.192 | 1.31× | 2,016 | **0** |
| 4,194,304 | 1,771.1 µs | 14,473.1 µs | 2,313.2 µs | 0.160 | 0.152 – 0.173 | 1.31× | 8,160 | **0** |

‡ This is the batched K = 1000 path (M1 §4 caveat), and 6 pairs can't resolve N ≤ 256; §14 has the dedicated small-N A/B. The reverse-order pair agrees with the forward runs (M1/M2 at 1M: 3,326 / 610 µs, faults 2,016 / 0). The canonical `pxir_bench_vector_add` (3 pairs) gives M1 3.20–3.28 ms and M2 0.56–0.60 ms, with the same checksum.

**Measured.** At N = 1M the M2 executor is 5.5× faster than M1 under default glibc on this machine, and 1.31× native. That is much more than the model alone predicts (24 → 16 B, 1.5×), because the page faults also disappeared (§11). It is an environment-specific result, not a general property of PXIR.

## 10. T1 results (faults removed in both builds)

With `GLIBC_TUNABLES=glibc.malloc.trim_threshold=1073741824:glibc.malloc.mmap_threshold=33554432`, over 3 alternating pairs:

| N | Native (B runs) | M1 total | M2 total | M2/M1 | Per pair | M2/native | Faults (M1/M2) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 65,536 | 7.83 µs | 22.7 µs | 13.5 µs | 0.60 | 0.54 – 0.72 | 1.73× | 0 / 0 |
| 1,048,576 | 453.7 µs | 899.8 µs | 592.8 µs | 0.66 | 0.56 – 0.72 | 1.31× | 0 / 0 |
| 4,194,304 | 1,814.5 µs | 3,733.2 µs | 2,416.5 µs | 0.65 | 0.63 – 0.67 | 1.33× | 0 / 0 |

At N = 1M under T1 (M2 tree; medians of 3 runs):

| Component | Median |
|---|---:|
| `result_buffer_create` | 149.8 µs |
| `add_loop_preallocated` | 466.5 µs |
| `result_create_plus_add` | 605.0 µs |
| `output_materialization` (copy) | 305.7 µs |
| `data_path_replica` (copy path) | 917.7 µs (A: 902.5 µs) |
| `data_path_move_replica` (move path) | 605.0 µs |
| **`pxir_execution_total` (M2)** | **592.8 µs** (M1: 899.8 µs) |

With faults removed on both sides, removing the copy takes M2/M1 to about 0.66, which matches the 16/24 = 0.667 byte model. Both executors move their model bytes at about the same rate: 28.0 GB/s for M1 (25.2 MB in 899.8 µs) and 28.3 GB/s for M2 (16.8 MB in 592.8 µs).

## 11. Page-fault analysis

| N | M1 faults/call | M2 faults/call (default) | M1 (T2) | M2 (T2) |
|---:|---:|---:|---:|---:|
| 65,536 | 96 | 0 | 130 | 65 |
| 1,048,576 | 2,016 | 0 | 2,050 | 1,025 |
| 4,194,304 | 8,160 | 0 | 8,194 | 4,097 |

T2 is `GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072`, which sends every block of 128 KiB or more through mmap (one pair). Its totals: M1 163.0 µs / 3,424 µs / 14,130 µs, and M2 82.7 µs / 1,719 µs / 7,877 µs.

- **Measured:** under default glibc, M2 has zero faults per call at every N (6 of 6 pairs, plus the reverse run and Clang). The move replica also has zero, while the copy replica, measured in the same B runs, still has 96 / 2,016 / 8,160. Under T2, M2 faults once per page of its single buffer (N/1024 + 1), exactly half of M1.
- **Inferred:** with one 4 MiB block freed per call instead of two adjacent ones, the free space at the top of the heap stays below glibc's dynamic trim threshold, so the memory is kept and reused. This is consistent with M1's isolated `result_buffer_create`, which allocates and frees one block and had zero faults. It was not checked against glibc's source.
- **Allocator dependence:** the fault reduction depends strongly on the allocator. Default glibc gives 0 faults; forced mmap gives half of M1's faults, and M2 then takes 1.72 ms instead of 0.60 ms at N = 1M.

## 12. strace comparison

The same probe ([`strace_probe.cpp`](data/m2/strace_probe.cpp): 10 executor calls at N = 1M, each result destroyed at the end of its iteration) was run with `strace -f -e trace=brk,mmap,munmap,madvise`:

| Build | Memory syscalls in 10 calls | Pattern |
| --- | --- | --- |
| M1 (A) | 30 `brk`, 0 others | Every call: +0x412000 (4 MiB + 72 KiB on the first call, then +4 MiB), +4 MiB, then −8 MiB |
| M2 (B) | **1 `brk`**, 0 others | First call: +0x412000 (4 MiB + 72 KiB); calls 2–10: **no memory syscalls** |

The traces are in [`strace_A_n1048576.txt`](data/m2/strace_A_n1048576.txt) and [`strace_B_n1048576.txt`](data/m2/strace_B_n1048576.txt).

## 13. Data-movement model

User-level bytes per element, with RFO and cache-line effects not modelled (as in M1):

| Stage | M1 | M2 (single output, final use) |
| --- | ---: | ---: |
| Result construction (value-initialize C) | 4 | 4 |
| Add (read A, read B, write C) | 12 | 12 |
| Output | 8 (read C, write copy) | **0 N-scaled payload bytes** (constant-size ownership transfer) |
| **Minimum** | **24** | **16** |
| Native (preallocated) | 12 | 12 |

The output transfer is not free: it costs about 11–15 ns at N ≤ 256 and 0.07–0.6 µs at larger N (K = 1, near the 26–30 ns timer floor, with no growth trend in N). It includes allocating the one-element `outputs` array. What goes away is the N-sized payload copy, together with the second N-sized allocation.

When a value has several outputs, or is read again after its output, the earlier outputs still copy. Such programs move 8 B/element for each extra copy, as in M1.

## 14. Small-N overhead

| Measurement | Result |
| --- | --- |
| `last_use_analysis` isolated (K = 1000) | 20–23 ns per call, independent of N |
| Full executor, N = 1 (10 pairs, 201 iterations each) | M1 156.9 ns, M2 163.4 ns; paired difference median **+6.3 ns** (range −12.9 to +26.9); M2 slower in 8 of 10 pairs |
| Full executor, N = 256 (10 pairs) | M1 265.9 ns, M2 260.4 ns; paired difference median −6.2 ns (range −32.6 to +11.6); M2 slower in 4 of 10 pairs |
| `output_move_materialization` vs `output_materialization` at N = 1 | about 11–15 ns vs about 20–25 ns |

The small-N change is the sum of two measured effects: the added last-use table (about +21 ns) and the removed one-element copy (about −10 ns). The expected net is about +11 ns. The measured +6.3 ns at N = 1 is consistent with a small regression, but its range includes zero. This is the batched path (M1 §4), so it is not canonical one-call allocator behavior. **The regression is reported, not hidden, and it has not been optimized.**

## 15. Correctness

- **Oracle:** every timed result that computes something is compared exactly with the independent oracle (bitwise for non-NaN values, any NaN matching any NaN, as in M0). All 49 benchmark output files in `docs/data/m2/` are free of `MISMATCH`.
- **Checksums:** at every N, the output checksums of M1 (GCC), M2 (GCC) and M2 (Clang 18.1.3) are identical. For example, N = 1M gives `0xf09ed3431c02ceea`, the same as M0 and M1.
- **New semantic tests** (`tests/execution/output_move_test.cpp`, suite `pxir_output_move_test`); cases A–G also check that the caller's input buffers are unchanged:

| Case | Program | Checks |
| --- | --- | --- |
| A | `output C` | Correct result |
| B | `output C; output C` | Both correct and identical (copy, then move) |
| C | `output C; D = C + A; output D` | Both correct (copy before a later use) |
| D | `output A; output A+B; output B` | Inputs returned by copy; caller buffers unchanged |
| E | `D = C + C; output D` | Exact |
| F | i32 `output C; output C` | Both equal the i32 oracle; inputs unchanged |
| G | Five outputs mixing moves and copies | Exact IR order and values |
| — | Same program executed 3 times | Identical results every call |

- **Mutation test** (temporary, not committed):
  1. **Move any owned value at output, ignoring last use.** Killed. `pxir_output_move_test` fails B, C, F, G and the repeated-execution case: the second read of the value fails closed with an internal "unbound" error. The M0 test `chained_adds_and_outputs_in_order` fails too.
  2. **Extra: also keep `bound` pointing at the moved-from buffer.** Killed by the same cases. In the tested libstdc++ environment (GCC 13.3), the moved-from Buffer appeared empty, so the later output returned an empty result. That is an observation from this environment, not a portable C++ guarantee. The portable conclusion is that reading a moved-from Buffer here would rely on an unspecified state and is semantically invalid. Clearing `bound` makes such a bug fail closed instead: with `slot.reset()` and `bound[id] = nullptr` (§4), PXIR never relies on moved-from contents.

  The implementation was restored and all 15 tests pass.

## 16. Threats to validity

- **Everything from M1 §14 still applies:** a shared virtualized host, no frequency control, a large reported L3 (the rates are consistent with cache residency, but hardware traffic was not measured), OS scheduling, one compiler family, the timer floor, auto-vectorization on both sides, and a single machine.
- **Allocator specificity:** the disappearance of faults under default glibc is **allocator-dependent** (§11, T2). It says nothing about macOS, Windows, jemalloc, mimalloc, other glibc versions or long-running processes. Only one T2 pair was run.
- **A/B binary differences:** A and B are different binaries (different code layout), and B's benchmark runs three extra components before `pxir_execution_total` in forward order, which changes heap history. The reverse-order pair, where `pxir_execution_total` runs first in both, gives the same conclusion.
- **Small-N resolution:** differences of a few nanoseconds are at the noise floor even with 10 pairs, and N ≤ 256 uses the batched lifetime path.
- **K = 1 constant-cost samples** such as `output_move_materialization` at large N (0.07–0.6 µs) are close to the timer floor and cold-cache effects. They show that the cost does not grow with N; they are not precise values.
- **Replica fidelity:** `data_path_move_replica` and `last_use_analysis` are hand-written equivalents that could drift from the executor. The replica matches the executor total within 3% at N = 65,536 and 1M, and within 6% at N = 4M (2,433.5 vs 2,313.2 µs).

## 17. Findings

**MEASURED**

- **Copy removed:** for `output C`, the N-sized output copy is gone. Moving the output costs 0.07–0.6 µs with no growth in N, against 300 µs (1M) and 1.2 ms (4M) for the copy. strace shows one heap growth instead of two per call.
- **Default glibc:** M2/M1 is 0.105 at N = 65,536, 0.182 at 1M and 0.160 at 4M. Page faults per call go from 96 / 2,016 / 8,160 to 0 / 0 / 0. At N = 1M, M2 is 1.31× native (596.6 vs 454.0 µs).
- **T1:** M2/M1 is 0.60–0.66, and at N = 1M M2 (592.8 µs) ≈ `result_create_plus_add` (605.0 µs).
- **T2 (forced mmap):** M2 still faults 1,025 times per call at 1M, half of M1.
- **strace:** M1 makes 3 `brk` calls per call; M2 makes one `brk` on the first call and no memory syscalls afterwards.
- **Small N:** `last_use_analysis` costs 20–23 ns in isolation. The full executor changes by +6.3 ns at N = 1 (8 of 10 pairs slower, range crossing zero) and −6.2 ns at N = 256.
- **Correctness:** all semantic tests, both mutations and every oracle check behave as expected, and checksums are identical across M1, M2 and Clang.

**INFERRED**

- The M1 faults came from two same-sized buffers being freed together. With one buffer they no longer cross glibc's trim threshold on this configuration. The causal link is strongly supported (a single variable was changed and the faults vanished; strace confirms), but glibc's internal logic was not traced.
- Under T1, the remaining M2 − native gap at N = 1M (about 140 µs) matches the measured zero-fill of the result (`result_buffer_create`, 149 µs).

**FALSIFIED**

- **Page faults persist after removing the copy.** Falsified for default glibc 2.39 here: 0 faults per call. It holds only under forced mmap (T2), and even there the count halves.
- **The output move copies payload at any size.** Falsified: its cost does not grow from N = 4,096 to 4M.

**NOT YET KNOWN**

- Fault behavior with other allocators and operating systems (macOS and Windows CI were not measured).
- Whether the small-N regression is real at about 5 ns. It needs higher resolution than 10 pairs.
- Actual cache and DRAM traffic (no hardware counters).
- Performance of programs with several outputs or reused values beyond correctness (only semantics were tested).

## 18. Next experiment

Recommended M3 experiment: **remove value-initialization of the executor's result buffer, so that `add` writes each element exactly once, and measure with this same A/B harness against M2.**

- **Why this one:** after M2, the zero-fill is the largest measured cost left on the canonical path. It costs about 149 µs at N = 1M, which matches the remaining M2 − native gap of about 140 µs, and it accounts for 4 of the 16 model B/element (16 → 12, the same as native).
- **Semantic risk:** the result's elements would briefly be uninitialized before the kernel writes them. Every element must be provably written before anything can observe it, and the oracle, the i32 path and the sanitizers must stay green.
- **Status:** not implemented.

# PXIR M6: Last-Use In-Place Result Reuse

**Status:** runtime optimization experiment. Branch `pxir/m6-last-use-inplace-reuse-v01`, base `main` = `bd338bae72c63677b533ed0c6e9cea1986358412` (M0–M5). Recommendation: **MERGE CANDIDATE**, conditional on green cross-platform CI (§25).

Labels: **MEASURED** was observed here; **INFERRED** is reasoning from measurements; **FALSIFIED** was contradicted; **NOT YET KNOWN** was not established.

## 1. Research question

Can PXIR reuse executor-owned storage at an operand's last use, so the second result allocation disappears, while keeping two computational passes and the 24 B/element user-level payload of `D = A + B; E = D + C`?

M6 is **not** fusion. There are still two loops. The second pass is `d[i] = d[i] + c[i]`, not `e[i] = (a[i] + b[i]) + c[i]`.

## 2. M5 motivation

M5 (T1, N = 1M) put the PXIR chain at 0.95 ms, equal to a native two-pass loop (0.94 ms). Under default glibc the same chain cost 2.69 ms with 2,016 minor faults per call, because two result-sized buffers are allocated and freed together and glibc trims and re-faults them. M6 removes one of the two allocations and nothing else, to separate *allocation/lifetime* from *materialization traffic*.

## 3. Hypotheses and outcome

| # | Hypothesis | Outcome |
| --- | --- | --- |
| H1 | Default final-only: faults → ~0, time → ~M5 T1 (0.95 ms) | **Supported, and exceeded.** 2,016 → 0 faults; 2.90 ms → 0.80 ms (0.28×). Time is *below* M5's T1 (see H2). |
| H2 | T1 unchanged (payload unchanged) | **FALSIFIED.** T1 final-only 0.966 → 0.797 ms (0.83×) at 1M, 0.82× at 4M. The payload model is unchanged, so this is not reduced traffic. The isolating microbenchmark (§13) shows the in-place destination itself is ~0.84× an out-of-place second pass with everything preallocated. |
| H3 | T2 final-only: 2,050 → 1,025 faults | **Supported** exactly. |
| H4 | Intermediate output: T2 3,075 → 2,050 | **Supported** exactly. |
| H5 | Output-after-use control behaves like M5 | **Supported.** 1.006× (default), equal faults. |
| H6 | Single add unchanged | **Supported** (1.04× default forward, 0.99× reverse; equal faults). |

## 4. Eligibility rule

For an `add` at operation index `i`, an operand `v` is reusable iff

```text
owned[v].has_value()  AND  last_use[v] == i
```

- Caller inputs are never in `owned`, so they can never be reused.
- `last_use` is the M2 table, built over the operands of **all** operations including `output`, so a later `output v` blocks reuse.
- An earlier `output v` does not block it: that output is an independent deep copy.
- Selection when both are eligible: **lhs, else rhs, else allocate.** There is no two-buffer coalescing, and the unselected dead operand is not reclaimed early (it stays owned until the call returns, as in M5).
- Same value on both sides (`D + D`) is one eligible value, reused as both operands.

## 5. Lifetime and ownership model

Order (hard invariant, `src/runtime/cpu_reference.cpp`):

1. `D` stays in `owned[D]`; `bound[D]` and the operand views remain valid.
2. The in-place kernel runs; all reads of element `i` precede the write of element `i`.
3. Only then: `owned[E] = std::move(owned[D])`.
4. `owned[D].reset()`.
5. `bound[D] = nullptr`.
6. `bound[E] = &*owned[E]`.

The buffer is never moved before it is read, no stale `bound` pointer survives, and after the operation exactly one ValueId owns the storage. If the kernel rejects the operands (scalar/length disagreement, checked before any write), the executor returns `internal_invariant_violation` with nothing modified. Fallback (no eligible operand) is the M4 path unchanged.

## 6. Phase A

`docs/data/m6/phase_a/inplace_kernel.cpp` is independent of the runtime (only `OwnedArray` and the oracle): `destination[i] = destination[i] + other[i]` for f32, `wrapping_add` for i32.

- **Correctness (MEASURED):** D = A + B into an `OwnedArray`, then D = D + C in place, against the oracle's `(A+B)+C`, at N ∈ {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 4099}; signed zero, ±inf, NaN (existing any-NaN policy), denormals, i32 wrap boundaries, and destination == other (`d[i] = d[i] + d[i]`). Passes with GCC and Clang `-O3 -Werror` and with GCC ASan + UBSan.
- **Vectorization (MEASURED, `-O3`, no `-march`, no fast-math):** GCC 13.3 f32 and i32: 16-byte vectors (`addps` / `paddd`) with a runtime alias check, then an 8-byte and a scalar epilogue. Clang 18.1 f32 and i32: width 4, interleave 2. No calls in either kernel (`phase_a/vectorization_reports.txt`, `disasm_*.txt`).
- **Gate:** all criteria met → runtime integration.

## 7. Runtime implementation

- `add_in_place<T>` (three loops chosen by `Reuse::{lhs, rhs, both}`): `d[i] = add(d[i], o[i])`, `add(o[i], d[i])`, `add(d[i], d[i])`. Operand order follows the IR in the source, but the compiler is free to commute an IEEE addition, so the source order is not a bit-level guarantee. The only observable effect is the payload of a NaN result, which IEEE-754 does not specify and which the oracle ignores (any NaN matches any NaN). An audit probe (not committed; five reuse shapes, 2,000 programs of 64 f32 elements with random NaN payloads, `-O2`) found no difference in any non-NaN element. Clang 18.1 matched M5 bit for bit; GCC 13.3 differed from M5 only for right-operand reuse (48,187 of 128,000 elements, all NaN versus NaN). M5 itself already differs between GCC and Clang, so NaN payloads were never portable across compilers.
- `add_buffers_in_place(Buffer& destination, const Buffer& lhs, const Buffer& rhs)`: destination identity is by address (`&lhs == &destination`), scalar type and length are checked first, no allocation.
- Executor: eligibility, then the transition in §5.

Files changed: `src/runtime/cpu_reference.cpp`, `include/pxir/runtime/buffer.hpp` (a friend declaration; §8), `include/pxir/runtime/cpu_reference.hpp` (comment), tests.

## 8. Mutable access design

No public mutable API was added. `Buffer` declares `friend struct detail::RuntimeBufferAccess`, a struct defined only in `cpu_reference.cpp`. Its `mutable_elements<T>(Buffer&)` returns a mutable span **only** for `OwnedArray`-backed storage; a vector-backed buffer (how caller inputs are stored) yields `nullopt`. So even a bug that selected an input as a destination would fail the kernel's precondition rather than write. `OwnedArray` is unchanged.

## 9. Alias safety

| Case | Handling |
| --- | --- |
| destination == lhs | `d[i] = d[i] + o[i]`; `o` is a different buffer. |
| destination == rhs | `d[i] = o[i] + d[i]`; IR order kept. |
| lhs and rhs the same value | `both` loop, `d[i] = d[i] + d[i]`: both reads of `i` precede the write of `i`; exact overlap is safe. |
| both operands owned, one selected | lhs is selected; the other is neither modified nor reclaimed. |
| output copy exists before reuse | The copy is a deep copy of D made earlier; reusing D's storage cannot change it (test B). |
| later output | `last_use` is the output, so no reuse (test C). |
| destination overlapping `o` | Impossible: distinct owned buffers or a caller input; deep copies. |

## 10. Correctness

`tests/execution/inplace_reuse_test.cpp` (against the independent oracle, N = 4,099, plus a black-box count of result-sized heap allocations per execution by replacing global `operator new`):

| Case | Program | Result allocs (M5 → M6) |
| --- | --- | --- |
| A final-only | `D=A+B; E=D+C; out E` | 2 → **1** |
| B output before use | `D=A+B; out D; E=D+C; out E` (D unchanged) | 3 → **2** |
| C output after use | `D=A+B; E=D+C; out D; out E` | 2 → 2 (reuse blocked) |
| D right operand | `E=C+D` | 2 → **1** |
| E same operand twice | `E=D+D` | 2 → **1** |
| F borrowed inputs | `A+B`, `A+A`, `C+G` | 3 → 3; inputs unchanged |
| both owned | `D=A+B; F=C+G; E=D+F` | 3 → **2** |
| later computational use | `D=A+B; E=D+C; F=D+E` | 3 → **2** (F reuses D) |
| longer chain | 4 dependent adds | 4 → **1** |
| repeated execution | same VerifiedProgram ×4 | identical, inputs unchanged |
| i32 | lhs, rhs, same-twice reuse; wrap boundaries | exact |
| f32 specials | −0, ±inf, NaN, denormals, both sides | exact |

Run against the M5 tree, the same test fails **only** the allocation-count checks (12 of them), never a value check: the counts are what distinguish the two runtimes. Every test asserts input buffers are unchanged.

## 11. Mutation tests (not committed)

1. **Premature reuse** (`last_use` ignored): killed. `output D` after the reuse finds D unbound and the executor returns `internal_invariant_violation` (the unbind invariant is a second barrier that fires before the symptom "output D holds the wrong data" can occur); 2 test failures (`docs/data/m6/mutations/mutation1_ignore_last_use.txt`).
2. **Borrowed-input reuse** (ownership gate removed; a `const_cast` on a non-const test input, giving the vector-backed accessor mutable spans, with the caller buffer moved into the result): killed by 31 failures, including input-modified checks and allocation counts (`mutation2_borrowed_input_reuse.txt`). The test's inputs are non-const objects, so the temporary mutation contains no UB; it was reverted and never committed.

An earlier version of the test crashed (segfault) under mutation 1 because it indexed `outputs` after a failed check; the test now returns early. The crash still counted as a kill, but is not acceptable evidence.

## 12. Benchmark methodology

Primary evidence is the **unmodified M5 benchmark** (`pxir_bench_intermediate_materialization`, byte-identical source in both trees) built from two clean trees: A = canonical M5 `bd338ba`, B = M6. Its PXIR components follow each tree's runtime.

- Harness: `steady_clock`, 5 warmup, 51 iterations, median; K = 1000 batching below N = 4,096; faults from `getrusage`; results checked outside the timed region.
- Order: for every run, A then B (interleaved). GCC default: 6 runs, plus 3 reverse-order; T1: 3; T2: 2; Clang default: 3, T1: 2; small N (N = 1, 256): 10 paired runs per compiler.
- T1: `GLIBC_TUNABLES=glibc.malloc.trim_threshold=1073741824:glibc.malloc.mmap_threshold=33554432`. T2: `glibc.malloc.mmap_threshold=131072`.
- Same machine class as M1–M5 (shared 4-vCPU cloud VM, Linux 6.18, glibc 2.39, GCC 13.3, Clang 18.1). Tables: `docs/data/m6/summary_tables.txt` (from raw `A_*.txt` / `B_*.txt` with `aggregate.py`).
- All results in the raw files (3388 `correctness=exact` lines, none other) report `correctness=exact`; checksums equal in A and B and across compilers (N = 1M: D `0xf09ed3431c02ceea`, E `0x166ecc1712346145`).

## 13. Default A/B (GCC, medians, µs; M6/M5)

| N | single add | final-only chain | intermediate-output | output-after-use |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1.05 | 0.96 | 0.97 | 1.05 |
| 256 | 1.04 | 1.03 | 0.97 | 1.03 |
| 4,096 | 1.00 | 1.00 | 0.93 | 1.06 |
| 65,536 | 1.00 | 0.84 | 0.78 | 0.91 |
| 1,048,576 | 1.04 | **0.28** | 0.75 | 1.01 |
| 4,194,304 | 1.00 | **0.23** | 0.64 | 0.99 |

N = 1M detail:

| Program | M5 µs | M6 µs | M6/M5 | M5 faults/call | M6 faults/call |
| --- | ---: | ---: | ---: | ---: | ---: |
| single add | 465 | 483 | 1.04 (reverse-order 0.99) | 0 | 0 |
| final-only chain | 2,896 | **803** | **0.28** | 2,016 | **0** |
| intermediate-output | 4,543 | 3,423 | 0.75 | 3,040 | 2,016 |
| output-after-use | 2,899 | 2,916 | 1.01 | 2,016 | 2,016 |

N = 4M: final-only 14.87 → 3.38 ms (8,160 → 0 faults); intermediate 25.9 → 16.5 ms (12,256 → 8,160). Reverse-order and Clang agree (final-only 0.28 / 0.28 at 1M). At 65,536 the final-only chain is 0.84× under default and T1 alike (0 faults in both), i.e. a cache-resident-size effect, not an allocator one.

The intermediate-output chain now behaves like M5's *final-only* chain: it still has two result-sized live buffers (D, the copy of D), so it still trims and re-faults (2,016 faults).

## 14. T1 (M6/M5, GCC, N = 1M / 4M)

| Program | 1M M5 → M6 µs | ratio | 4M ratio |
| --- | ---: | ---: | ---: |
| single add | 487 → 477 | 0.98 | 1.02 |
| final-only | 966 → **797** | **0.83** | 0.82 |
| intermediate-output | 1,316 → 1,131 | 0.86 | 0.90 |
| output-after-use | 972 → 950 | 0.98 | 1.04 |

No page faults in any T1 run. M6 final-only (797 µs) sits between native fused (631–655 µs) and native two-pass (953–959 µs). Clang T1 agrees (0.84 / 0.83).

The controls (single add, output-after-use) are unchanged; only reuse-eligible programs speed up. **INFERRED:** the T1 gain is a destination effect, since the payload model is identical. `inplace_microbench.cpp` (all arrays preallocated and pre-touched, no allocation at all) measures the second pass alone: in-place `D = D + C` vs out-of-place `E = D + C`: in-place/out-of-place = 0.84–0.89 at 1M and 0.83–0.87 at 4M (both compilers, 3 runs), and ~0.82 already at N = 4,096 (one GCC outlier run at 0.55). A consistent explanation is that writing back into lines that were just read avoids a write-allocate read of fresh destination lines and shrinks the pass-2 working set from three arrays to two; **hardware counters were not available, so this mechanism is not measured** (NOT YET KNOWN).

## 15. T2 and page faults (faults/call; M5 → M6)

| N | single add | final-only | intermediate-output | output-after-use |
| ---: | ---: | ---: | ---: | ---: |
| 1M | 1,025 → 1,025 | 2,050 → **1,025** | 3,075 → **2,050** | 2,050 → 2,050 |
| 4M | 4,097 → 4,097 | 8,194 → **4,097** | 12,291 → **8,194** | 8,194 → 8,194 |

Fault sets equal the number of first-written result-sized buffers: exactly one set per allocation removed. Time at 1M (T2): final-only 3.06 → 1.75 ms, intermediate 4.74 → 3.42 ms.

Default (glibc trim) faults at 1M: final-only 2,016 → 0; intermediate 3,040 → 2,016; the others unchanged.

## 16. strace (10 calls, N = 1M; `docs/data/m6/strace/`, probe `m6_probe.cpp`)

| Program | M5 default | M6 default | M5 T2 | M6 T2 |
| --- | --- | --- | --- | --- |
| final-only | repeated `brk` grow/shrink cycles (12 `brk` calls in the process) | 4 `brk` calls in the whole process, no grow/shrink cycle | 20 mmap + 20 munmap of 4,198,400 B (2/call) | 10 + 10 (1/call) |
| intermediate-output | repeated `brk` grow/shrink (38 calls) | 11 calls: the same cycling as M5's final-only | 30 + 30 (3/call) | 20 + 20 (2/call) |
| output-after-use | `brk` cycling (11 calls) | identical to M5 (11 calls) | 20 + 20 | 20 + 20 |

Probe fault counts (per call, default): final-only 915.8 → 205.3 (M6 pays a first-call fault set only), intermediate 2,944.6 → 813.4, output-after-use 813.3 → 813.3. **Caveat:** this probe's M5 final-only count (915.8) does not reproduce the 2,019.5 recorded by M5's own probe and benchmark, although both use three 4 MiB inputs; glibc's trim behavior depends on allocation history (known from M5 §17). The benchmark, not the probe, is the fault evidence; the probe supports the qualitative syscall pattern and the T2 counts, which are exact.

## 17. Logical peak owned payload (model, not RSS)

| Program | M5 | M6 |
| --- | ---: | ---: |
| final-only | 8 B/element (D + E) | **4** |
| intermediate-output | 12 (D, D copy, E) | **8** (D reused as E, D copy) |
| output-after-use | 8 | 8 (D and E must coexist) |
| single add | 4 | 4 |

## 18. Data-movement model

Unchanged: final-only 24 B/element in both M5 and M6 (pass 1: read A, B, write D = 12; pass 2: read D, C, write D = 12). Intermediate-output remains 32 (the D copy is semantically required). M6 removes an allocation and reduces peak storage; it does **not** remove "write D, read D". A model of 16 would mean accidental fusion; the code has two loops (§19).

## 19. GCC integrated codegen

Actual `cpu_reference.cpp`, `-O3 -DNDEBUG`, no `-march`, no fast-math, no intrinsics or pragmas (`docs/data/m6/gcc_integrated_*`, `integrated_*`): all three in-place loops (`lhs`, `rhs`, `both`) are reported "vectorized using 16 byte vectors" (and 8-byte epilogues); `lhs` and `rhs` are versioned for possible aliasing. Per-instantiation attribution (probe: only `noinline` added to the template, loop bodies unchanged): f32 has `addps` (5) and no calls; i32 has `paddd` (3) and, for `d[i]+d[i]`, `pslld` (2), i.e. the same value as doubling, produced by the compiler; no per-element calls.

## 20. Clang integrated codegen

Clang 18.1: all three in-place loops "vectorized loop (vectorization width: 4, interleaved count: 2)"; f32 `addps` (6), i32 `paddd` (6), no calls (`clang_integrated_probe_vec_remarks.txt`).

## 21. Small N (10 paired runs; median ns, M5 → M6)

| N | Program | GCC | Clang |
| ---: | --- | --- | --- |
| 1 | single add | 177 → 184 (+6, 8/10 slower) | 194 → 194 (+0.5) |
| 1 | final-only | 251 → 243 (−8) | 266 → 250 (−16) |
| 1 | output-after-use | 281 → 291 (+10, 8/10) | 289 → 295 (+6, 9/10) |
| 256 | single add | 273 → 285 (+13, 9/10) | 289 → 295 (+7, 8/10) |
| 256 | final-only | 360 → 370 (+10, 9/10) | 371 → 374 (+3) |
| 256 | output-after-use | 433 → 445 (+12, 9/10) | 448 → 455 (+7, 8/10) |

Non-reusing programs are consistently a little slower (about +3 % to +5 %, +6 to +13 ns): the added eligibility checks and code layout. It was not optimized away. Reusing programs at N = 1 are faster; at N = 256 GCC is slightly slower (+10 ns) because K = 1000 batching amortizes M5's allocation.

## 22. Longer-chain control

Correctness (test): four dependent adds allocate **one** result buffer and match the oracle. Supplementary measurement (`longer_chain/`, `m6_probe chain4`, N = 1M, five inputs, 3 runs × 51 calls): per-call faults default 340 → 40, T1 100 → 20, T2 4,100 → 1,025 (**four allocations → one**); time default 1.97 → 1.48 ms, T1 1.95 → 1.46 ms, T2 6.4 → 2.5 ms (0.75× / 0.75× / 0.39×). The reduction scales with chain depth. This is supplementary only; the two-add workload is canonical.

## 23. Threats to validity

- One shared cloud VM, one glibc (2.39); nothing for other allocators, macOS, or Windows.
- No hardware counters: cache and write-allocate explanations (§14) are inference.
- The default-allocator gain depends on glibc's trim heuristics and allocation history; the probe/benchmark difference in §16 shows this.
- Sizes N ≥ 1M are DRAM/L3-sensitive on a shared host; noise up to a few percent (see the ±4 % on the single-add control).
- Valgrind was not run on `inplace_reuse_test` (its replaced `operator new` conflicts with Valgrind's interposition); it was run on the chain, execution, output-move and buffer-storage tests (clean) and the new test ran under ASan + UBSan.
- macOS and MSVC were not built locally; CI on a PR has not run.
- Small-N: the non-reusing path is 3–5 % slower.

## 24. Findings

**MEASURED**
- Final-only chain: faults 2,016 → 0 and time 2.90 → 0.80 ms at 1M under default glibc; T2 fault sets 2 → 1; strace mmap/munmap 2 → 1 pairs per call.
- Intermediate-output chain: 3 → 2 T2 fault sets, 4.5 → 3.4 ms (default).
- Controls (single add, output-after-use) unchanged in time and faults.
- T1 final-only is 0.83× M5, and the preallocated microbenchmark shows in-place ≈ 0.84× out-of-place.
- Both compilers vectorize all in-place loops (f32 and i32).
- Longer chains scale: 4 allocations → 1.
- The non-reusing path is 3–5 % slower at small N.

**INFERRED**
- The default-glibc cost in M5 was mostly the second result allocation's trim/re-fault (M6 removes it and lands at 0.80 ms, below M5's T1).
- The T1 gain is a destination-line reuse effect (fewer lines touched, no write-allocate on E), not payload reduction.

**FALSIFIED**
- "T1 stays approximately unchanged" (H2): 17 % faster.
- "M6 T1 ≈ M5 T1 implies allocation alone was the cost": the M5 T1 baseline itself contained a destination-locality cost.

**NOT YET KNOWN**
- The hardware mechanism behind the in-place speed-up.
- Behavior with other allocators/OSes, and with longer or wider programs (reclaiming unreused dead operands early).

## 25. Merge recommendation

**MERGE CANDIDATE**, once CI on the PR is green on Linux, macOS and Windows/MSVC and the ASan+UBSan job (not run in this environment). Criteria: exact correctness (§10); no UB (ASan/UBSan, Valgrind on the runtime tests); inputs never modified (asserted in every test, mutation 2); earlier output copies correct; later outputs block reuse; lhs, rhs, same-value correct; repeated execution stable; no double ownership (single owner after transfer; §5); GCC and Clang integrated loops vectorize; primary result non-regressive (0.28× final-only, controls unchanged); small-N regression ≤ 13 ns and confined to non-reusing programs; one-variable scope preserved (no fusion, no early reclamation, no allocator change, no IR/verifier change, no public API change: one friend declaration, no new public symbol).

## 26. Candidate M7

**Fusion of a single-use intermediate.** M6 leaves exactly one known cost in the final-only chain: pass 2's re-read of D (24 → 16 B/element). With allocation and reuse solved, M7 would evaluate `E = (A + B) + C` in one loop when D's only use is the following add (same evaluation order, no reassociation), and measure it against the M6 baseline (T1 ≈ 0.80 ms) and native fused (≈ 0.63 ms) under default, T1 and T2. Not implemented in M6.

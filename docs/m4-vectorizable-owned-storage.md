# Lume M4: Vectorizable Single-Write Owned Storage

M4 asks whether Lume can own result storage that is **not value-initialized**, write every result element **exactly once** through a **plain indexed loop**, and keep compiler auto-vectorization. That is the combination M3 could not achieve with `std::vector`.

The experiment ran in two phases:
- **Phase A:** the storage primitive in isolation, with a go/no-go gate.
- **Phase B:** integration into `Buffer` and the executor, which happened only because Phase A passed.

M4 was measured with the M1–M3 method against canonical `main` on the same machine.

Every number is from one machine (glibc 2.39, GCC 13.3.0, Clang 18.1.3, libstdc++ 13, baseline x86-64 without `-march`). None of it is a general performance claim.

## 1. Research question

Can a standard-conforming owning storage representation remove the M2 value-initialization while keeping a compiler-friendly indexed loop?

Secondary questions:
- whether GCC and Clang vectorize f32 and i32;
- whether the zero-fill disappears and the model is 12 B/element;
- M4 against M2 and against native at large N;
- cache-resident sizes;
- T1 and T2;
- output copy and move semantics;
- suitability as a foundation for later SIMD and GPU work.

## 2. Canonical M2 baseline

Canonical `main` is `9514215c170e6d9df601e4280d86a73f70cb5104`: the M2 runtime plus the M3 research record. The M2 add builds `std::vector<T>(n)` (a zero-fill), then runs an indexed loop that GCC and Clang vectorize. Output of a final-use result moves it. Model: 16 B/element (4 zero-fill + 12 add). At N = 1M, M2 was about 0.63 ms against native's 0.47 ms.

## 3. M3 lesson

M3 (`reserve` + `emplace_back`) removed the zero-fill, but neither compiler vectorized the append loop. Even scalar against scalar, it cost about 2× the indexed loop. Fewer bytes do not help if the abstraction destroys the hot loop. M4 therefore keeps the loop exactly as M2 wrote it (`c[i] = a[i] + b[i]`) and changes only the storage underneath it.

## 4. Hypotheses (stated before measuring)

| Id | Hypothesis | Outcome |
| --- | --- | --- |
| H1 | Storage created with `make_unique_for_overwrite<T[]>` removes the zero-fill | **Supported.** `owned_array_allocate_for_overwrite` costs 41–72 ns at every N ≥ 4,096 (default, reverse and T1 runs), against `result_buffer_create` at 157 µs at N = 1M. |
| H2 | GCC and Clang vectorize the single-write indexed loop, both in isolation and integrated | **Supported.** Both compilers, f32 and i32, Phase A and Phase B (§20–21). |
| H3 | Under T1, M4/M2 ≈ 12/16 = 0.75 (not required) | **Supported.** 0.73 at N = 1M and 0.76 at N = 4M. |
| H4 | M4 approaches native at large N | **Supported.** M4/native is 1.01 at 1M and 0.98 at 4M (default allocator). |
| H5 | Under T2, M4 keeps M2's demand-zero page faults | **Supported.** 1,025 faults per call at 1M in both builds, yet M4 is faster (1.37 vs 1.92 ms). |
| H6 | The new representation adds small-N cost | **Mixed.** GCC: neutral (N = 1 +1.6 ns, M4 slower in 6/10 pairs; N = 256 −5.8 ns, 3/10). Clang: small, consistent regression (N = 1 +9.0 ns, 9/10; N = 256 +4.2 ns, 8/10). |

## 5. Phase A design

The primitive is `lume::OwnedArray<T>` ([`include/lume/runtime/owned_array.hpp`](../include/lume/runtime/owned_array.hpp)):
- a contiguous, owning, fixed-size array restricted to `float` and `std::int32_t`;
- storage is a `std::unique_ptr<T[]>` plus a `std::size_t`;
- `OwnedArray<T>::for_overwrite(n)` calls `std::make_unique_for_overwrite<T[]>(n)`. Where that library feature is missing (`__cpp_lib_smart_ptr_for_overwrite` undefined), it falls back to its specified equivalent, `std::unique_ptr<T[]>(new T[n])`. The fallback was tested separately under ASan+UBSan (§12).

No custom allocator, pool, arena, alignment change, over-allocation or SIMD type is involved. The kernel under test is exactly:

```cpp
auto c = OwnedArray<float>::for_overwrite(a.size());
for (std::size_t i = 0; i < a.size(); ++i) c[i] = a[i] + b[i];        // i32: wrapping_add(a[i], b[i])
```

## 6. C++ object-lifetime analysis

- **Element lifetime:** `new T[n]`, which is what `make_unique_for_overwrite<T[]>` is, *default-initializes* the array. For the trivially default-constructible scalars `float` and `int32_t`, this starts the lifetime of all n elements and leaves their values indeterminate (C++20 [dcl.init]/[basic.life]). The elements are objects; nothing is written through storage whose lifetime hasn't started.
- **No indeterminate reads:** reading an indeterminate value of these types is undefined behavior, so the invariant is that *every element is assigned before anything reads it*. The kernel assigns `c[0..n)` in order and never reads `c`. The array is wrapped in a `Buffer` only after the loop completes, and no other code sees it before then.
- **Enforced type properties:** the class `static_assert`s `is_trivially_default_constructible`, `is_trivially_destructible` and `is_trivially_copyable`, and restricts `T` to `float` and `int32_t`.
- **Evidence that no unwritten element is read:**
  - Valgrind Memcheck, which tracks the definedness of every byte, reports 0 errors across all 7 runtime test executables.
  - In the incomplete-write mutation (§12), Memcheck reported 17 "Conditional jump … depends on uninitialised value(s)" errors. So it does detect such reads here.
  - GCC ASan+UBSan are clean.
  - Clang's MemorySanitizer is not available in this environment (no compiler-rt).
- **Exception safety:** `for_overwrite` may throw `bad_alloc` or `bad_array_new_length` before any element exists. The kernel's loop cannot throw. On an exception the local `OwnedArray` is destroyed and the exception propagates out of `execute_cpu_reference` exactly as before M4.

## 7. OwnedArray design

| Operation | Semantics |
| --- | --- |
| `for_overwrite(n)` | n elements, indeterminate values (the only way to create a non-empty array) |
| Copy construction | allocates with `for_overwrite(size)`, then `std::copy_n` every element (a deep copy; it reads only fully written sources) |
| Copy assignment | copy-and-swap; strong exception guarantee; self-assignment-safe |
| Move construction and assignment | transfer the `unique_ptr`; the source becomes `{nullptr, 0}`; `noexcept` (static-asserted) |
| `size()`, `data()`, `operator[]` (unchecked), `view()` → `std::span<const T>` | read and write access; `view()` is the representation-neutral read form |

`sizeof(OwnedArray<float>)` is 16 bytes. `sizeof(Buffer)` is 32 bytes in both M2 and M4: the variant did not grow.

## 8. Phase A vectorization gate

The Phase A probe is [`docs/data/m4/phase_a_kernel_probe.cpp`](data/m4/phase_a_kernel_probe.cpp). The full evidence is in [`vectorization_evidence.txt`](data/m4/vectorization_evidence.txt).

| Loop | GCC 13.3 | Clang 18.1 |
| --- | --- | --- |
| f32 | Vectorized, 16-byte vectors (`addps`); *no* runtime aliasing check was needed | Vectorized, width 4, interleave 2 |
| i32 | Vectorized, 16-byte vectors (`paddd`) | Vectorized, width 4, interleave 2 |

The GCC f32 hot loop is two `movups` loads, `addps`, a `movups` store, then add, compare and branch. The only call is `operator new[]`, before the loop. There are no per-element capacity checks, size updates or calls.

**Phase A performance gate** (3 runs, isolated components):

| N | M2 `result_create_plus_add` | M4 `owned_array_allocate_plus_add` | Ratio |
|---:|---:|---:|---:|
| 4,096 | 798 ns | 750 ns | 0.94 |
| 65,536 | 13.7 µs | 10.7 µs | 0.78 |
| 1,048,576 | 624.5 µs | 455.5 µs | 0.73 |
| 4,194,304 | 2,449.8 µs | 1,827.7 µs | 0.75 |

The raw runs are `docs/data/m4/phaseA_run*.txt`.

**Decision: GO.** Correctness exact, ASan+UBSan and Memcheck clean, no zero-fill, and both compilers vectorize both loops. There is no large-N regression: the ratio is 0.73–0.75.

## 9. Phase B Buffer integration

`Buffer` storage became `std::variant<std::vector<float>, std::vector<int32_t>, OwnedArray<float>, OwnedArray<int32_t>>`.
- **Caller inputs:** still constructed from `std::vector`, moved in, never copied into an `OwnedArray`.
- **Executor results:** `OwnedArray`-backed.

`add_buffers` keeps the Phase A loop shape exactly: span operands, then `for_overwrite`, then an indexed single write, then `Buffer(std::move(c))`. Unchanged: the IR, the verifier, typed ids, validation, the last-use table, the output move and unbind, and `ExecutionResult`.

## 10. API representation boundary

This is an explicit, **breaking** public API change:

| | Before (M0–M3) | After (M4) |
| --- | --- | --- |
| Construction | `Buffer(std::vector<float>)`, `Buffer(std::vector<int32_t>)` | Same, plus `Buffer(OwnedArray<float>)` and `Buffer(OwnedArray<int32_t>)` |
| Read access | `const std::vector<float>* as_f32()`, `const std::vector<int32_t>* as_i32()` (nullptr = other type) | **Removed.** Replaced by `std::optional<std::span<const float>> f32_view()` and `std::optional<std::span<const std::int32_t>> i32_view()` (nullopt = other type) |
| `scalar()`, `length()` | Unchanged | Unchanged |

- **Reason:** `as_f32` exposed `std::vector` as the storage type. Keeping it would have required returning `nullptr` for a *valid* f32 buffer backed by an `OwnedArray`, which would be a silent semantic trap. Spans give one read interface for both kinds of storage, with no allocation and no copy.
- **Migration:** every internal user was migrated (the executor, both benchmarks, the execution and output-move tests). External code must replace `*b.as_f32()` with `*b.f32_view()`. A span is not a vector: code that compared with `==` against a `std::vector`, or kept a `std::vector&`, needs `std::equal` or a copy.
- **Cost of the views (inspected):** the views are built once per `add`, outside the loop. The integrated hot loop has the same shape as M2's (§20).

## 11. Ownership, copy and move invariants

| Invariant | Evidence |
| --- | --- |
| Copies are deep for all four storage kinds | `buffer_storage_test`: same type, length and bits, with distinct storage pointers, for vector f32/i32 and OwnedArray f32/i32, by copy construction and copy assignment |
| `Buffer` move is `noexcept` and keeps storage identity | `static_assert(is_nothrow_move_constructible_v<Buffer>)` and `is_nothrow_move_assignable_v`; tests for move construction, move assignment and growth of a `std::vector<Buffer>` |
| Final-use output moves the executor's result; earlier outputs copy | `buffer_storage_test` (distinct storage for copy then move) and `output_move_test` |
| Inputs are borrowed; an output of an input is a copy | Caller storage pointer and values unchanged after execution; the output of an input has different storage |
| After a move, the slot is reset and the value unbound | Unchanged M2 code |

## 12. Correctness

- **New tests:**
  - `lume_owned_array_test` (Phase A kernel): f32 and i32 at N ∈ {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 4099}; f32 special values (±0, ±∞, NaN, max, denormals, ties-to-even) at the start, middle and tail; i32 wrap boundaries; deep copies, copy assignment and self-assignment; moves and vector growth.
  - `lume_buffer_storage_test` (Phase B): the integrated executor at every edge length for f32 and i32; copies of all four storage kinds; wrong-type views; moves; output ownership.
  - The existing M0–M2 tests cover chained computation, output-twice, output before a later use, input passthrough and i32 wrap-around through the executor.
- **Results:** all 17 tests pass (GCC Release `-Werror`, GCC ASan+UBSan, Clang Release `-Werror`). Valgrind Memcheck: 0 errors in all 7 runtime tests. The forced `new T[n]` fallback path passes `owned_array` and `buffer_storage` under ASan+UBSan.
- **Mutation 1** (temporary, not committed): the executor loops were changed to `i + 1 < size`, leaving the last element unwritten. It was killed by 5 tests: `execution`, `output_move`, `buffer_storage`, `vector_add` smoke and `executor_breakdown` smoke. Memcheck reported the unwritten element being read (17 errors).
- **Mutation 2** (temporary, not committed): an aliasing `OwnedArray` copy (`data_(other.data_.get())`).
  - The normal `-Werror` build refuses it (GCC `-Wuse-after-free`).
  - Built without `-Werror`, it was killed by the `owned_array` copy-independence checks (shared storage, and writing the source changed the copy) and by the `buffer_storage` distinct-storage checks.
  - 4 test executables aborted with "free(): double free detected".
  - An earlier attempt at this mutation was discarded: its build failed under `-Werror`, so the tests had run stale binaries.
- **Oracle and checksums:** no `MISMATCH` in any of the 75 benchmark output files in `docs/data/m4/`. Checksums are identical between M2 and M4 (GCC and Clang) at every N; N = 1M gives `0xf09ed3431c02ceea`.

## 13. Benchmark methodology

This uses the M1/M2 harness and statistics unchanged (see [`m1-executor-cost-breakdown.md`](m1-executor-cost-breakdown.md) §4, including the small-N batching caveat).
- **Historical controls keep their code:** `result_buffer_create`, `add_loop_preallocated`, `result_create_plus_add` and `data_path_move_replica` are unchanged.
- **One control input changed:** because `Buffer` no longer exposes `std::vector`, the isolated vector-form kernels now read the generator vectors `a` and `b` (same values; a separate allocation from the executor's input Buffers) instead of the Buffers' internal vectors.
- **New components:**
  - `owned_array_allocate_for_overwrite`: allocation only; 0 payload B/element.
  - `owned_array_indexed_add`: the kernel into existing storage; 12.
  - `owned_array_allocate_plus_add`: M4's `add_buffers` data path; 12.
  - `data_path_owned_array_replica`: allocate, write once, wrap in an owned slot, move into a fresh outputs vector; 12.
- **`pxir_execution_total`** follows this tree's executor; its model is 12 B/element.

**A/B:**
- **A** = canonical `main` `9514215` (separate worktree); **B** = M4. Both use CMake Release `-O3 -DNDEBUG` with `-DLUME_WARNINGS_AS_ERRORS=ON`, on the same machine and in one session.
- Strictly alternating runs:
  - default allocator: 6 pairs (N ∈ {1, 256, 4,096, 65,536, 1M, 4M});
  - T1: 3 pairs;
  - T2: 1 pair;
  - reverse order: 1 pair;
  - small-N: 10 pairs × 201 iterations (GCC) and 10 pairs (Clang);
  - `vector_add`: 3 pairs;
  - Clang full A/B: 2 pairs.
- A first matrix was discarded before analysis because it predated the final benchmark code (`data_path_owned_array_replica` and the 12 B/element model). Every committed file is from the final binaries.

Raw data is in [`docs/data/m4/`](data/m4/) (`A_*` = M2, `B_*` = M4).

## 14. Default allocator

`pxir_execution_total`, GCC, 6 pairs:

| N | Native | M2 | M4 | M4/M2 | Per pair | M4/native | Faults M2 / M4 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 ‡ | 1.1 ns | 171 ns | 172 ns | 1.01 | 0.65 – 1.10 | — | 0 / 0 |
| 256 ‡ | 26.3 ns | 272 ns | 266 ns | 0.98 | 0.93 – 1.02 | 10.1× | 0 / 0 |
| 4,096 | 594 ns | 873 ns | 833 ns | 0.95 | 0.91 – 1.04 | 1.40× | 0 / 0 |
| 65,536 | 8.52 µs | 14.5 µs | 8.71 µs | 0.60 | 0.55 – 0.86 | 1.02× | 0 / 0 |
| 1,048,576 | 469.4 µs | 619.2 µs | 472.8 µs | **0.76** | 0.73 – 0.80 | **1.01×** | 0 / 0 |
| 4,194,304 | 1,911.9 µs | 2,548.4 µs | 1,877.6 µs | **0.74** | 0.71 – 0.80 | **0.98×** | 0 / 0 |

‡ These are measured on the batched K = 1000 path; see §22.

Other runs agree:
- **Clang A/B** (2 pairs): M4/M2 = 0.91 (4,096), 0.62 (65,536), 0.74 (1M), 0.74 (4M). M4/native is 0.96 at 1M.
- **Reverse-order pair** (M2 / M4): 1M 649.6 / 470.4 µs; 4M 2,545.6 / 1,851.1 µs.
- **`lume_bench_vector_add`** (3 pairs): M2 0.62–0.63 ms, M4 0.45–0.47 ms, with the same checksum.

### Canonical N = 1,048,576 (GCC, default allocator; B-tree components, medians of 6)

| Component | Path | Median |
|---|---|---:|
| `native_add_preallocated` | — | 469.4 µs |
| `result_buffer_create` (zero-fill) | M2 control | 157.2 µs |
| `add_loop_preallocated` (indexed, vectorized) | M2 control | 472.8 µs |
| `result_create_plus_add` | M2 control | 634.7 µs |
| `owned_array_allocate_for_overwrite` | M4 | **45 ns** |
| `owned_array_indexed_add` (vectorized) | M4 | 466.3 µs |
| `owned_array_allocate_plus_add` | M4 | 462.3 µs |
| `data_path_move_replica` | M2 control | 626.6 µs |
| `data_path_owned_array_replica` | M4 | 466.6 µs |
| `pxir_execution_total` (A = M2) | M2 | 619.2 µs |
| `pxir_execution_total` (B = M4) | M4 | **472.8 µs** |

The zero-fill is gone (157 µs → 45 ns), and the loop is unchanged (473 vs 466 µs). M4 − M2 = −146 µs, close to the 149–157 µs zero-fill that M2 identified.

## 15. T1 (`trim_threshold=1 GiB`, `mmap_threshold=32 MiB`; 3 pairs)

| N | Native | M2 | M4 | M4/M2 | Per pair |
|---:|---:|---:|---:|---:|---:|
| 65,536 | 8.31 µs | 14.2 µs | 8.94 µs | 0.63 | 0.62 – 0.64 |
| 1,048,576 | 476.7 µs | 634.7 µs | 461.4 µs | 0.73 | 0.70 – 0.74 |
| 4,194,304 | 1,878.1 µs | 2,530.1 µs | 1,920.2 µs | 0.76 | 0.76 – 0.78 |

Neither build faults under default glibc (since M2), so T1 changes nothing. The byte model predicted 0.75; at large N the measurement is 0.73–0.76.

## 16. T2 (`mmap_threshold=128 KiB`; 1 pair)

| N | M2 total (faults) | M4 total (faults) | M4 `allocate_for_overwrite` (faults) |
|---:|---:|---:|---:|
| 65,536 | 91.9 µs (65) | 73.0 µs (65) | 3.1 µs (1) |
| 1,048,576 | 1,917 µs (1,025) | 1,373 µs (1,025) | 3.3 µs (1) |
| 4,194,304 | 8,679 µs (4,097) | 6,819 µs (4,097) | 3.2 µs (1) |

With fresh mmap'd pages, the kernel's demand-zero work remains in both builds: the fault count is identical and moves into the add loop for M4. M4 removes only the user-level zero-fill, and is still 24–29% faster.

## 17. Page faults

| N | Default M2 / M4 | T1 M2 / M4 | T2 M2 / M4 |
|---:|---:|---:|---:|
| 65,536 | 0 / 0 | 0 / 0 | 65 / 65 |
| 1,048,576 | 0 / 0 | 0 / 0 | 1,025 / 1,025 |
| 4,194,304 | 0 / 0 | 0 / 0 | 4,097 / 4,097 |

## 18. strace

Probe [`docs/data/m2/strace_probe.cpp`](data/m2/strace_probe.cpp): 10 executor calls at N = 1M. The traces are in `docs/data/m4/strace_*`.

| Build | Default glibc | T2 |
|---|---|---|
| M2 (A) | 1 `brk`, then no memory syscalls | 10 × (`mmap` 4,198,400 B + `munmap`) |
| M4 (B) | 1 `brk`, then no memory syscalls | 10 × (`mmap` 4,198,400 B + `munmap`) |

Allocation sizes and lifetimes are unchanged, and so is allocator syscall behavior.

## 19. Data movement

| Stage | M2 | M4 |
| --- | ---: | ---: |
| Result construction | 4 (value-initialize) | 0 (allocation only) |
| Add: read A, read B, write C | 12 | 12 |
| Output (single, final use) | 0 N-scaled | 0 N-scaled |
| **Minimum user-level B/element** | **16** | **12** (equal to native) |

- **Measured:** at N ≥ 1M, M4 streams its 12 model B/element at the native rate (M4/native 0.98–1.01), and M4/M2 (0.73–0.76) is close to the 12/16 byte ratio.
- **Not measured:** actual DRAM or cache traffic (there were no hardware counters), RFO, and the kernel's demand-zero writes (only visible under T2).

## 20. GCC vectorization (integrated, Phase B)

`src/runtime/cpu_reference.cpp`, the f32 loop at line 60 and the i32 loop at line 69:
- f32: "loop vectorized using 16 byte vectors" (plus an 8-byte epilogue); `addps`.
- i32: the same, with `paddd`.

Both are versioned at runtime for possible aliasing, as in M2: once `add_buffers` is inlined into `execute_cpu_reference`, GCC no longer proves the fresh allocation cannot alias the inputs, which it did in the isolated probe. The main loop is the same shape as M2's: `movups`, `movups`, `addps`, `movups`, then add, compare and branch. There are no allocation or capacity checks, size updates or calls inside the loop.

## 21. Clang vectorization (integrated, Phase B)

f32 (line 60) and i32 (line 69): "vectorized loop (vectorization width: 4, interleaved count: 2)". The object contains `addps` and `paddd`. It is the same result as Clang on M2's loops.

Phase A and Phase B agree for both compilers. The representation boundary (`Buffer`, `std::variant`, `std::optional<std::span>`) did not prevent vectorization.

## 22. Small N

Dedicated paired A/B, 10 pairs × 201 iterations, batched K = 1000 path:

| Compiler | N | M2 | M4 | Paired difference (median, range) | M4 slower in |
|---|---:|---:|---:|---:|---:|
| GCC | 1 | 168.5 ns | 173.6 ns | +1.6 ns (−73.3 to +44.1) | 6 / 10 |
| GCC | 256 | 273.4 ns | 268.3 ns | −5.8 ns (−12.1 to +11.4) | 3 / 10 |
| Clang | 1 | 178.5 ns | 190.1 ns | **+9.0 ns** (−66.2 to +82.6) | **9 / 10** |
| Clang | 256 | 279.1 ns | 281.9 ns | +4.2 ns (−10.6 to +15.7) | 8 / 10 |

- **GCC:** neutral.
- **Clang:** a small, consistent regression (about 5% at N = 1). Its cause was not isolated. Plausible sources are the 4-way `variant` visits in `scalar()`, `length()` and the views, and the `optional<span>` construction. This is inferred and has not been measured.

## 23. Threats to validity

- **Everything from M1–M3 still applies:** one shared VM, no frequency control, a large reported L3 (so large-N rates are consistent with cache residency but not proven), no hardware counters, allocator dependence (glibc 2.39), the timer floor (26–30 ns), and small-N batching.
- **Toolchain:** GCC 13.3 and Clang 18.1 with libstdc++ 13 only. `make_unique_for_overwrite` implementations differ (the libc++ and MSVC STL paths were not executed here), and the fallback path was tested only on libstdc++.
- **Code layout:** A and B are different binaries, and the B benchmark has extra components. The reverse-order pair agrees.
- **Control input change:** the isolated vector-form controls read `a`/`b` instead of the input Buffers' storage (§13). At N = 65,536, isolated M4 components (11.2–11.4 µs) are slower than the M4 executor total (8.7 µs). That is a cache-state difference between separately allocated inputs, and it shows isolated components at L2-sized working sets should not be read as exact shares of the total.
- **Cross-platform CI:** macOS (libc++) and Windows (MSVC) were **not** built locally (libc++ is not installed and there is no MSVC). CI runs only on pull requests and pushes to `main`, so it has not run on this branch.
- **Public API change:** `as_f32`/`as_i32` were removed (§10). The benefit is weighed against the break in §25.
- **Baseline ISA:** no `-march`. AVX2 and AVX-512 were not used, and wider vectors were not tested.

## 24. Findings

**MEASURED**

- The storage primitive (`make_unique_for_overwrite<T[]>`) removes the zero-fill: allocation costs about 45 ns, against about 157 µs for the zero-fill at N = 1M.
- GCC 13.3 and Clang 18.1 vectorize the single-write indexed loop for f32 and i32, in isolation and integrated (GCC: 16-byte `addps`/`paddd`; Clang: width 4, interleave 2).
- Large N:
  - GCC default allocator: M4/M2 is 0.76 at 1M and 0.74 at 4M; M4/native is 1.01 and 0.98.
  - T1: M4/M2 is 0.73 and 0.76.
  - Clang: M4/M2 is 0.74 and 0.74.
  - `vector_add`: 0.62 → 0.46 ms.
- T2: fault counts are identical, but M4 is 24–29% faster. Allocator syscalls are identical.
- Small N: neutral with GCC; a small, consistent regression with Clang (+9 ns at N = 1).
- Correctness:
  - all 17 tests pass;
  - ASan+UBSan, Valgrind Memcheck and the forced fallback path are clean;
  - both mutations are killed;
  - checksums are identical;
  - `sizeof(Buffer)` is unchanged at 32 bytes.

**INFERRED**

- The remaining M2 − native gap was the zero-fill; with it removed, the executor is limited by the same data movement as native on this machine.
- The Clang small-N cost comes from representation dispatch (variant and optional), not from the kernel.

**FALSIFIED**

- The M3 reading that "single-write construction inherently loses vectorization with standard C++". It does with `emplace_back`, and it does not with non-value-initialized owned storage plus indexed writes.

**NOT YET KNOWN**

- macOS/libc++ and Windows/MSVC build and behavior (CI has not run).
- Whether other compilers, standard libraries or target ISAs keep vectorization.
- The exact cause of the Clang small-N regression.
- Actual DRAM and cache traffic.

## 25. Merge recommendation

**MERGE CANDIDATE**, with one explicit gate that is still open: **cross-platform CI (Linux, macOS/libc++, Windows/MSVC) must pass on the pull request before merging.** It could not be verified locally.

Every other criterion is met:
- correctness is exact; no undefined behavior (ASan, UBSan, Memcheck); M2 semantics preserved;
- GCC and Clang vectorize both integrated loops;
- there is no severe small-N regression (GCC neutral, Clang +9 ns or about 5% at N = 1, reported above);
- large-N performance improves (0.73–0.76× M2, about native).

The API change removes `as_f32`/`as_i32` in favor of span views. It is judged acceptable for a pre-1.0 research codebase, because keeping the vector-typed accessors would silently misreport valid `OwnedArray`-backed buffers. It is a breaking change and must be called out in the merge.

## 26. Candidate M5

Recommended M5 experiment: **establish a multi-operation baseline, e.g. `E = (A + B) + C` with and without output of the intermediate, and measure the cost of materializing intermediate values.**

- **What to measure:** traffic, time and faults against a native single-pass loop, with the same harness.
- **Why this one:** with M4, a single add already runs at native speed, so single-operation executor overhead is no longer the question. The next Lume-relevant cost is data movement between operations. Every intermediate currently makes a full N-sized round trip through memory.
- **What it prepares:** measuring this first, per the project rule of establishing a baseline before optimizing, would size any later fusion experiment.
- **Status:** not implemented.

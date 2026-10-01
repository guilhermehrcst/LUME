# Lume M0: Minimal Executable IR

## Goal

Represent one computation, `C = A + B` over `f32[N]`, independently of how it is executed. Then verify it, run it on a scalar CPU reference executor, and compare the result with an independent native baseline. M0 establishes a baseline. It is not an optimization.

```text
C++ builder API  ->  Program (core IR)  ->  verify()  ->  VerifiedProgram  ->  execute_cpu_reference()
                                               |                                     |
                                          Diagnostics                  outputs == native oracle?
```

## Supported

- Scalar types `f32` (IEEE-754 binary32) and `i32` (two's complement; add wraps modulo 2^32).
- Fixed-length one-dimensional buffer types such as `f32[1024]`.
- Operations `input`, `add`, `output`.
- A verifier that is mandatory: only a `VerifiedProgram` can be executed.
- A scalar CPU reference executor.
- Correctness tests, negative tests, and a baseline benchmark.
- A human-readable debug dump (`lume::to_debug_string`).

## Not supported

Textual Lume language or parser, LLVM, CUDA, SIMD intrinsics, optimization passes, JIT, tensor ranks or dynamic shapes, type inference, AI workloads, and a `lume-inspect` CLI. There is no input format to inspect yet, so M0 ships only the in-memory dump function.

## Example

```cpp
#include "lume/ir/dump.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"

lume::Program program;
auto a = program.input(lume::f32, 1024);
auto b = program.input(lume::f32, 1024);
auto c = program.add(a, b);
program.output(c);

std::string text = lume::to_debug_string(program);

lume::VerifyResult verified = lume::verify(std::move(program));
if (!verified.ok()) { /* inspect verified.diagnostics */ }

std::vector<lume::Buffer> inputs{lume::Buffer(std::vector<float>(1024, 1.0f)),
                                 lume::Buffer(std::vector<float>(1024, 2.0f))};
lume::ExecutionResult result = lume::execute_cpu_reference(*verified.program, inputs);
// result.outputs[0].f32_view() -> 1024 x 3.0f  (M0-M3 API: as_f32())
```

The debug dump for this program:

```text
%0 = input f32[1024]
%1 = input f32[1024]
%2 = add %0, %1
output %2
```

The dump exists for humans. It is not a language, and there is no parser for it.

## IR design

| Concept | Representation |
| --- | --- |
| `ValueId`, `TypeId`, `OperationId` | Distinct 32-bit index types. Construction from an integer is explicit, and they don't convert into each other. `0xFFFFFFFF` is the invalid sentinel, and a default-constructed id is invalid. |
| `ScalarType` | `uint8_t` enum with `f32 = 1` and `i32 = 2`. Zero is reserved, so zero-initialized memory is detectably invalid. |
| `Type` | `{ScalarType scalar; uint32_t length;}`. Types are interned in a table, so identical types share a `TypeId`. |
| `Value` | `{TypeId type; OperationId definer;}`, one SSA value per producing operation. |
| `Operation` | `{Opcode opcode; ValueId operands[2]; ValueId result;}`. Unused slots hold the invalid id. |
| `Program` | Three `std::vector` tables (`types`, `values`, `operations`) in creation order. There are no heap pointers between IR entities. |
| `Buffer` | A runtime host buffer: `std::variant<std::vector<float>, std::vector<int32_t>>`. |

The builder records exactly what it is asked to record and never rejects input. The verifier is the single authority on validity. `Program::from_storage` adopts raw tables without checking them, so the verifier can be tested against states the builder cannot produce.

Measured sizes (x86-64, GCC 13.3 and Clang 18.1):

| Structure | Bytes |
| --- | ---: |
| `ValueId` / `TypeId` / `OperationId` | 4 |
| `Type` | 8 |
| `Value` | 8 |
| `Operation` | 16 (1 opcode byte + 3 padding + 2 x 4 operands + 4 result) |
| `Program` object | 72 (three vectors) |
| IR tables for the vector add (1 type, 3 values, 4 ops) | 96 used, 104 reserved |

The IR is not bit-packed. Packing belongs to a later milestone and needs measurement to justify it.

## Verifier invariants

Diagnostics come out in deterministic order: tables, types, values, operations, then program. Each one carries a `DiagnosticCode`, a location (operation, value or type id), and a message such as:

```text
error[shape_mismatch] op 2 (add): cannot add %0: f32[100] and %1: f32[200] (lengths differ)
```

| Code | Invariant |
| --- | --- |
| `table_too_large` | Every table has at most 2^32 - 1 entries, so all indices stay below the sentinel. |
| `invalid_scalar_type` | Each type's scalar is `f32` or `i32`. |
| `zero_length` | Each type's length is greater than 0. |
| `byte_size_overflow` | `length * element_size` fits in `size_t`. |
| `invalid_type_id` | Each value's type exists. |
| `invalid_definer` | Each value's defining operation exists. |
| `definer_mismatch` | A value's definer names that value as its result (value -> op -> value). |
| `invalid_opcode` | Each opcode is `input`, `add` or `output`. |
| `missing_operand` / `invalid_operand` / `unexpected_operand` | Operand arity per opcode is input 0, add 2, output 1. Every present operand exists. |
| `use_before_definition` | An operand is defined by a strictly earlier operation. This rules out self-reference and cycles. |
| `missing_result` / `invalid_result` / `unexpected_result` | `input` and `add` produce an existing value; `output` produces none. |
| `result_mismatch` | A result value's definer is this operation (op -> value -> op). |
| `scalar_type_mismatch` | `add` operands share a scalar type. |
| `shape_mismatch` | `add` operands share a length. |
| `result_type_mismatch` | An `add` result has the operand type. |
| `no_outputs` | The program has at least one `output`. |

Execution before verification cannot be represented. `VerifiedProgram` has a private constructor that only `verify()` can call, it exposes the program read-only, and the executor accepts nothing else. `static_assert`s in `tests/invalid` enforce this.

## Reference executor

`execute_cpu_reference(const VerifiedProgram&, std::span<const Buffer>)`:

1. It checks that the number of buffers equals the number of `input` operations. Then it checks each buffer's scalar type and length against its input's IR type, before any computation. Any mismatch returns an error with no outputs (fail closed).
2. It interprets operations in order. Inputs are borrowed without copying. Each `add` allocates a result buffer and runs a plain scalar loop (`c[i] = a[i] + b[i]`; for `i32`, the add goes through `uint32_t` to avoid signed-overflow UB). Each `output` copies its value into the result.
3. The kernel re-checks operand scalar type and length. A disagreement there would mean a Lume bug, so it returns `internal_invariant_violation` instead of reading out of bounds.

The executor is not optimized, and it contains no hand-written SIMD. The compiler may auto-vectorize its loops and the native baseline alike under the default Release flags.

## Correctness oracle

`oracle/include/lume_oracle/oracle.hpp` is structurally independent of the code under test. It includes no Lume header, and the `lume_oracle` CMake target does not link `lume`. It provides:

- **Deterministic inputs:** `std::mt19937_64` (whose sequence is fixed by the C++ standard; the standard distributions are not) mapped to exactly representable floats in `[-1, 1)` with 2^-23 spacing. A then B come from one seeded stream.
- **Native baseline:** a plain loop, written separately from the executor.
- **Comparison policy (`exactly_equal`):** every non-NaN result must match **bit for bit**, so `-0.0` differs from `+0.0`, and there is no tolerance. Both sides perform one correctly rounded binary32 addition on identical inputs, so any difference is a bug. The one exception is NaN, which matches any NaN. IEEE-754 does not specify the sign or payload of a NaN produced by an invalid operation, and they do differ in practice: at `-O2` Clang folds `inf + -inf` at compile time to `0x7fc00000`, while x86 hardware produces `0xffc00000`. That observation is what defined this policy.

## Benchmark

`benchmarks/vector_add.cpp` builds `lume_bench_vector_add [elements] [seed] [warmup] [iterations]`. The defaults are `1048576 42 5 51`. It records the workload, element count, dtype, seed, generator, warmup and iteration counts, compiler, and build config. It also records the IR dump, table counts, `sizeof` of the key structures, and IR storage bytes. For construction, verification, native execution and Lume execution it reports the median, min and max. It checks every execution against the oracle and prints FNV-1a checksums. It exits non-zero on any mismatch.

The Lume timed region covers input validation, interpretation, allocation of the result buffer, the add loop, and the output copy. The native timed region covers only the add loop into a preallocated buffer. The comparison is therefore **not like for like**. It measures what the reference executor costs today.

To reproduce, record `git rev-parse HEAD`, then:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/benchmarks/lume_bench_vector_add
```

### Baseline observation

Environment: Linux 6.18 container on a 4-vCPU Intel Xeon @ 2.10 GHz (shared cloud VM), GCC 13.3.0, CMake Release (`-O3 -DNDEBUG`), N = 1,048,576, seed 42, 5 warmup, 51 iterations.

| Measurement | Median |
| --- | ---: |
| Construction | 120 ns |
| Verification | 153 ns |
| Native add loop | 473,706 ns |
| Lume reference execution | 3,384,409 ns |
| Correctness | `exact`, checksum `0xf09ed3431c02ceea` |

Clang 18.1.3 produced the same checksum and similar medians (native 464,211 ns, Lume 3,337,261 ns). A second GCC run reproduced every non-timing field exactly.

This is one run on a shared virtual machine. It is not a performance claim. The roughly 7x gap is expected, since the Lume path does two extra 4 MiB allocate-and-write passes (a zero-initialized result and an output copy) that the native loop does not. That explanation is a hypothesis: M0 has not measured how the gap splits between those costs.

## Known limitations

- An id from one `Program` passed to another's builder is not detected when it happens to be in range.
- Only straight-line programs with no control flow. Operations have at most two operands.
- `table_too_large` and `byte_size_overflow` are implemented but untested. They need more than 4 billion entries or a 32-bit `size_t` to trigger.
- Type interning is a linear search, which is fine for M0-sized programs.
- The executor allocates per `add` and copies per `output`.
- Clang sanitizers were not run, because the development container lacks compiler-rt. GCC ASan+UBSan runs locally and in CI.

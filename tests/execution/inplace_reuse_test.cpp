// M6: last-use in-place result reuse. Correctness against the independent
// oracle for every reuse/no-reuse shape, plus a black-box allocation count:
// the number of result-sized heap allocations made during one execution is
// the observable that distinguishes reuse from a fresh result buffer.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "check.hpp"
#include "pxir/runtime/cpu_reference.hpp"
#include "pxir/verify/verifier.hpp"
#include "pxir_oracle/oracle.hpp"

namespace {

// Counts operator new / new[] calls of at least `g_threshold` bytes while
// armed. The tests use n = 4099 elements (16396 bytes), far above any
// executor bookkeeping allocation.
bool g_armed = false;
std::size_t g_threshold = 0;
std::size_t g_big_allocations = 0;

void* counted_alloc(std::size_t size) {
    if (g_armed && size >= g_threshold) ++g_big_allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}

}  // namespace

void* operator new(std::size_t size) { return counted_alloc(size); }
void* operator new[](std::size_t size) { return counted_alloc(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using pxir::Buffer;
using F = std::vector<float>;
using I = std::vector<std::int32_t>;

constexpr std::uint32_t kN = 4099;

pxir::VerifiedProgram verified(pxir::Program p) {
    pxir::VerifyResult r = pxir::verify(std::move(p));
    return std::move(*r.program);
}

struct Run {
    pxir::ExecutionResult result;
    std::size_t big_allocations = 0;
};

Run run(const pxir::VerifiedProgram& program, const std::vector<Buffer>& inputs) {
    g_threshold = kN * sizeof(float);
    g_big_allocations = 0;
    g_armed = true;
    Run r{pxir::execute_cpu_reference(program, inputs), 0};
    g_armed = false;
    r.big_allocations = g_big_allocations;
    return r;
}

F sum(const F& x, const F& y) {
    F out(x.size());
    pxir_oracle::native_add(x, y, out);
    return out;
}
I sum(const I& x, const I& y) {
    I out(x.size());
    pxir_oracle::native_add(x, y, out);
    return out;
}

bool is(const Buffer& b, const F& expected) {
    const auto v = b.f32_view();
    return v && pxir_oracle::exactly_equal(*v, expected);
}
bool is(const Buffer& b, const I& expected) {
    const auto v = b.i32_view();
    return v && v->size() == expected.size() && std::equal(v->begin(), v->end(), expected.begin());
}

struct Data {
    F a, b, c, g;
    I ia, ib, ic;
};

Data make_data(std::uint32_t seed) {
    std::mt19937_64 engine(seed);
    Data d;
    d.a = pxir_oracle::generate_f32(engine, kN);
    d.b = pxir_oracle::generate_f32(engine, kN);
    d.c = pxir_oracle::generate_f32(engine, kN);
    d.g = pxir_oracle::generate_f32(engine, kN);
    d.ia = pxir_oracle::generate_i32(engine, kN);
    d.ib = pxir_oracle::generate_i32(engine, kN);
    d.ic = pxir_oracle::generate_i32(engine, kN);
    return d;
}

std::vector<Buffer> f32_inputs(const Data& d) { return {Buffer(d.a), Buffer(d.b), Buffer(d.c), Buffer(d.g)}; }

pxir::Program four_f32_inputs(pxir::ValueId (&v)[4]) {
    pxir::Program p;
    for (auto& x : v) x = p.input(pxir::f32, kN);
    return p;
}

// Inputs are borrowed: their content must be unchanged after execution.
void inputs_unchanged(const std::vector<Buffer>& inputs, const Data& d) {
    PXIR_CHECK(is(inputs[0], d.a));
    PXIR_CHECK(is(inputs[1], d.b));
    PXIR_CHECK(is(inputs[2], d.c));
    PXIR_CHECK(is(inputs[3], d.g));
}

// A. D = A + B; E = D + C; output E.  One result buffer instead of two.
void final_only() {
    const Data d = make_data(1);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(p.add(dd, v[2]));
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    PXIR_CHECK(is(r.result.outputs[0], sum(sum(d.a, d.b), d.c)));
    PXIR_CHECK(r.big_allocations == 1);
    inputs_unchanged(in, d);
}

// B. D = A + B; output D; E = D + C; output E.  The output of D is an
// independent copy; D's storage is then reused for E.
void output_before_use() {
    const Data d = make_data(2);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(dd);
    p.output(p.add(dd, v[2]));
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 2)) return;
    const F expected_d = sum(d.a, d.b);
    PXIR_CHECK(is(r.result.outputs[0], expected_d));  // not clobbered by the reuse
    PXIR_CHECK(is(r.result.outputs[1], sum(expected_d, d.c)));
    PXIR_CHECK(r.big_allocations == 2);  // D and the output copy of D; E reuses D
    inputs_unchanged(in, d);
}

// C. D = A + B; E = D + C; output D; output E.  A later output blocks reuse.
void output_after_use() {
    const Data d = make_data(3);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    const auto e = p.add(dd, v[2]);
    p.output(dd);
    p.output(e);
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 2)) return;
    const F expected_d = sum(d.a, d.b);
    PXIR_CHECK(is(r.result.outputs[0], expected_d));
    PXIR_CHECK(is(r.result.outputs[1], sum(expected_d, d.c)));
    PXIR_CHECK(r.big_allocations == 2);  // D and E both exist
    inputs_unchanged(in, d);
}

// D. E = C + D: the reusable operand is the right-hand side; operand order
// (C + D) is preserved.
void right_operand() {
    const Data d = make_data(4);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(p.add(v[2], dd));
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    PXIR_CHECK(is(r.result.outputs[0], sum(d.c, sum(d.a, d.b))));
    PXIR_CHECK(r.big_allocations == 1);
    inputs_unchanged(in, d);
}

// E. E = D + D: one value is both operands; d[i] = d[i] + d[i].
void same_operand_twice() {
    const Data d = make_data(5);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(p.add(dd, dd));
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    const F expected_d = sum(d.a, d.b);
    PXIR_CHECK(is(r.result.outputs[0], sum(expected_d, expected_d)));
    PXIR_CHECK(r.big_allocations == 1);
    inputs_unchanged(in, d);
}

// F. E = A + B with caller-owned operands: nothing to reuse, inputs intact.
void borrowed_inputs() {
    const Data d = make_data(6);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    p.output(p.add(v[0], v[1]));
    p.output(p.add(v[0], v[0]));  // the last use of an input is still not a reuse
    p.output(p.add(v[2], v[3]));
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 3)) return;
    PXIR_CHECK(is(r.result.outputs[0], sum(d.a, d.b)));
    PXIR_CHECK(is(r.result.outputs[1], sum(d.a, d.a)));
    PXIR_CHECK(is(r.result.outputs[2], sum(d.c, d.g)));
    PXIR_CHECK(r.big_allocations == 3);  // each add allocates; each output moves
    inputs_unchanged(in, d);
}

// Both operands owned and at their last use: D = A + B; F = C + G; E = D + F.
// The lhs is reused; F is not reclaimed early (allocations stay at two).
void both_owned() {
    const Data d = make_data(7);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    const auto f = p.add(v[2], v[3]);
    p.output(p.add(dd, f));
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    PXIR_CHECK(is(r.result.outputs[0], sum(sum(d.a, d.b), sum(d.c, d.g))));
    PXIR_CHECK(r.big_allocations == 2);
    inputs_unchanged(in, d);
}

// D has a later computational use: E = D + C must not reuse D; the final add
// F = D + E then reuses D (its last use).
void later_computational_use() {
    const Data d = make_data(8);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    const auto e = p.add(dd, v[2]);
    p.output(p.add(dd, e));
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    const F expected_d = sum(d.a, d.b);
    PXIR_CHECK(is(r.result.outputs[0], sum(expected_d, sum(expected_d, d.c))));
    PXIR_CHECK(r.big_allocations == 2);  // D, E; F reuses D
    inputs_unchanged(in, d);
}

// V1 = A + B; V2 = V1 + C; V3 = V2 + G; V4 = V3 + A; output V4. One physical
// buffer travels through four ValueIds.
void longer_chain() {
    const Data d = make_data(9);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    auto x = p.add(v[0], v[1]);
    x = p.add(x, v[2]);
    x = p.add(x, v[3]);
    x = p.add(x, v[0]);
    p.output(x);
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    PXIR_CHECK(is(r.result.outputs[0], sum(sum(sum(sum(d.a, d.b), d.c), d.g), d.a)));
    PXIR_CHECK(r.big_allocations == 1);
    inputs_unchanged(in, d);
}

// Same VerifiedProgram, same inputs, repeatedly: identical results, inputs
// never modified (destructive updates touch executor-owned storage only).
void repeated_execution() {
    const Data d = make_data(10);
    auto in = f32_inputs(d);
    pxir::ValueId v[4];
    pxir::Program p = four_f32_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(dd);
    p.output(p.add(v[2], dd));
    const pxir::VerifiedProgram program = verified(std::move(p));
    const F expected_d = sum(d.a, d.b);
    const F expected_e = sum(d.c, expected_d);
    for (int k = 0; k < 4; ++k) {
        const Run r = run(program, in);
        if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 2)) return;
        PXIR_CHECK(is(r.result.outputs[0], expected_d));
        PXIR_CHECK(is(r.result.outputs[1], expected_e));
        PXIR_CHECK(r.big_allocations == 2);
        inputs_unchanged(in, d);
    }
}

// i32: reuse must keep modular wrap-around, on either side and twice.
void i32_cases() {
    const Data d = make_data(11);
    const std::vector<Buffer> in{Buffer(d.ia), Buffer(d.ib), Buffer(d.ic)};
    pxir::Program p;
    const auto a = p.input(pxir::i32, kN);
    const auto b = p.input(pxir::i32, kN);
    const auto c = p.input(pxir::i32, kN);
    const auto x = p.add(a, b);
    const auto y = p.add(x, c);     // lhs reuse
    const auto z = p.add(c, y);     // rhs reuse
    p.output(p.add(z, z));          // same operand twice
    const Run r = run(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    const I ez = sum(d.ic, sum(sum(d.ia, d.ib), d.ic));
    PXIR_CHECK(is(r.result.outputs[0], sum(ez, ez)));
    PXIR_CHECK(r.big_allocations == 1);
    PXIR_CHECK(is(in[0], d.ia) && is(in[1], d.ib) && is(in[2], d.ic));
}

void i32_wrap_boundaries() {
    constexpr std::int32_t hi = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t lo = std::numeric_limits<std::int32_t>::min();
    const I a{hi, lo, hi, -1, 0};
    const I b{1, -1, hi, 1, lo};
    const I c{hi, lo, 2, lo, lo};
    const std::vector<Buffer> in{Buffer(a), Buffer(b), Buffer(c)};
    pxir::Program p;
    const auto ia = p.input(pxir::i32, 5);
    const auto ib = p.input(pxir::i32, 5);
    const auto ic = p.input(pxir::i32, 5);
    p.output(p.add(p.add(ia, ib), ic));
    const auto r = pxir::execute_cpu_reference(verified(std::move(p)), in);
    if (!PXIR_CHECK(r.ok() && r.outputs.size() == 1)) return;
    PXIR_CHECK(is(r.outputs[0], I{-1, -1, 0, lo, 0}));
}

// f32 special values through the in-place path, both operand sides.
void f32_specials() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float den = std::numeric_limits<float>::denorm_min();
    const F a{-0.0f, inf, 1.0f, nan, -inf, den, -den, 0.0f};
    const F b{-0.0f, 1.0f, -inf, 1.0f, 5.0f, den, den, -0.0f};
    const F c{-0.0f, -inf, 2.0f, 0.0f, inf, den, -den, -0.0f};
    const std::vector<Buffer> in{Buffer(a), Buffer(b), Buffer(c)};
    for (const bool rhs : {false, true}) {
        pxir::Program p;
        const auto ia = p.input(pxir::f32, 8);
        const auto ib = p.input(pxir::f32, 8);
        const auto ic = p.input(pxir::f32, 8);
        const auto x = p.add(ia, ib);
        p.output(rhs ? p.add(ic, x) : p.add(x, ic));
        const auto r = pxir::execute_cpu_reference(verified(std::move(p)), in);
        if (!PXIR_CHECK(r.ok() && r.outputs.size() == 1)) return;
        PXIR_CHECK(is(r.outputs[0], rhs ? sum(c, sum(a, b)) : sum(sum(a, b), c)));
    }
}

}  // namespace

int main() {
    final_only();
    output_before_use();
    output_after_use();
    right_operand();
    same_operand_twice();
    borrowed_inputs();
    both_owned();
    later_computational_use();
    longer_chain();
    repeated_execution();
    i32_cases();
    i32_wrap_boundaries();
    f32_specials();
    return pxir_test::finish("inplace_reuse");
}

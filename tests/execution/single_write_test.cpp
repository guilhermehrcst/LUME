// M3: add results are built with reserve() + emplace_back(), one construction
// per element. These tests check the result length and every element against
// the oracle at lengths around common vector widths, for special values, for
// i32 wrap-around, for chained adds and together with the M2 output move.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include "check.hpp"
#include "pxir/runtime/cpu_reference.hpp"
#include "pxir/verify/verifier.hpp"
#include "pxir_oracle/oracle.hpp"

using pxir::Buffer;

namespace {

pxir::VerifiedProgram vector_add(pxir::ScalarType scalar, std::uint32_t n) {
    pxir::Program p;
    const auto a = p.input(scalar, n);
    const auto b = p.input(scalar, n);
    p.output(p.add(a, b));
    pxir::VerifyResult r = pxir::verify(std::move(p));
    return std::move(*r.program);
}

// Runs C = A + B and compares length and bit patterns with the oracle.
bool f32_matches_oracle(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> expected(a.size());
    pxir_oracle::native_add(a, b, expected);
    const std::vector<Buffer> inputs{Buffer(a), Buffer(b)};
    const auto r = pxir::execute_cpu_reference(vector_add(pxir::f32, static_cast<std::uint32_t>(a.size())), inputs);
    return r.ok() && r.outputs.size() == 1 && r.outputs[0].as_f32() != nullptr &&
           r.outputs[0].length() == a.size() && pxir_oracle::exactly_equal(*r.outputs[0].as_f32(), expected);
}

bool i32_matches_oracle(const std::vector<std::int32_t>& a, const std::vector<std::int32_t>& b) {
    std::vector<std::int32_t> expected(a.size());
    pxir_oracle::native_add(a, b, expected);
    const std::vector<Buffer> inputs{Buffer(a), Buffer(b)};
    const auto r = pxir::execute_cpu_reference(vector_add(pxir::i32, static_cast<std::uint32_t>(a.size())), inputs);
    return r.ok() && r.outputs.size() == 1 && r.outputs[0].as_i32() != nullptr &&
           r.outputs[0].length() == a.size() && *r.outputs[0].as_i32() == expected;
}

// A, B: lengths around 4-, 8- and 16-wide vectors, and a larger odd length.
void f32_lengths_around_vector_widths() {
    for (const std::uint32_t n : {1u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 33u, 4099u}) {
        std::mt19937_64 engine(n);
        const auto a = pxir_oracle::generate_f32(engine, n);
        const auto b = pxir_oracle::generate_f32(engine, n);
        if (!PXIR_CHECK(f32_matches_oracle(a, b))) std::fprintf(stderr, "  f32 n=%u\n", n);
    }
}

// C: special values placed at the start, middle and tail of a 17-element
// vector, so they are exercised in every position a vectorized kernel would
// split into (body and remainder).
void f32_special_values_at_every_position() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float max = std::numeric_limits<float>::max();
    constexpr float denorm = std::numeric_limits<float>::denorm_min();
    const std::vector<float> specials_a{0.0f, -0.0f, -0.0f, inf, max, denorm, nan, 1.0f, -inf};
    const std::vector<float> specials_b{-0.0f, -0.0f, 0.0f, -inf, max, denorm, 1.0f, 0x1p-24f, -inf};
    for (std::size_t offset : {std::size_t{0}, std::size_t{4}, std::size_t{8}}) {
        std::vector<float> a(17, 0.5f);
        std::vector<float> b(17, 0.25f);
        for (std::size_t k = 0; k < specials_a.size(); ++k) {
            a[(offset + k) % 17] = specials_a[k];
            b[(offset + k) % 17] = specials_b[k];
        }
        if (!PXIR_CHECK(f32_matches_oracle(a, b))) std::fprintf(stderr, "  specials offset=%zu\n", offset);
    }
    // Sign of zero is part of the exact policy.
    const std::vector<Buffer> inputs{Buffer(std::vector<float>{-0.0f}), Buffer(std::vector<float>{-0.0f})};
    const auto r = pxir::execute_cpu_reference(vector_add(pxir::f32, 1), inputs);
    PXIR_CHECK(r.ok() && std::signbit((*r.outputs[0].as_f32())[0]));
}

// D: i32 wrap boundaries at several lengths, plus random data.
void i32_wrap_boundaries() {
    constexpr std::int32_t hi = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t lo = std::numeric_limits<std::int32_t>::min();
    const std::vector<std::int32_t> a{hi, lo, hi, lo, -1, 1, 0, hi};
    const std::vector<std::int32_t> b{1, -1, hi, lo, 1, -1, lo, lo};
    PXIR_CHECK(i32_matches_oracle(a, b));

    const std::vector<Buffer> inputs{Buffer(a), Buffer(b)};
    const auto r = pxir::execute_cpu_reference(vector_add(pxir::i32, 8), inputs);
    if (PXIR_CHECK(r.ok())) {
        PXIR_CHECK((*r.outputs[0].as_i32() == std::vector<std::int32_t>{lo, hi, -2, 0, 0, 0, lo, -1}));
    }

    for (const std::uint32_t n : {1u, 5u, 9u, 17u, 4099u}) {
        std::mt19937_64 engine(1000 + n);
        const auto x = pxir_oracle::generate_i32(engine, n);
        const auto y = pxir_oracle::generate_i32(engine, n);
        if (!PXIR_CHECK(i32_matches_oracle(x, y))) std::fprintf(stderr, "  i32 n=%u\n", n);
    }
}

// E: C = A + B; D = C + A; output D. The second add reads a result built by
// the first.
void chained_add() {
    constexpr std::uint32_t n = 17;
    std::mt19937_64 engine(5);
    const auto a = pxir_oracle::generate_f32(engine, n);
    const auto b = pxir_oracle::generate_f32(engine, n);
    std::vector<float> c(n);
    std::vector<float> d(n);
    pxir_oracle::native_add(a, b, c);
    pxir_oracle::native_add(c, a, d);

    pxir::Program p;
    const auto va = p.input(pxir::f32, n);
    const auto vb = p.input(pxir::f32, n);
    p.output(p.add(p.add(va, vb), va));
    auto verified = pxir::verify(std::move(p));
    if (!PXIR_CHECK(verified.ok())) return;
    const std::vector<Buffer> inputs{Buffer(a), Buffer(b)};
    const auto r = pxir::execute_cpu_reference(*verified.program, inputs);
    PXIR_CHECK(r.ok() && r.outputs.size() == 1 && pxir_oracle::exactly_equal(*r.outputs[0].as_f32(), d));
}

// F: the M2 output rules on single-write results: copy, then move, and an
// output before a later use.
void m2_output_rules_still_hold() {
    constexpr std::uint32_t n = 9;
    std::mt19937_64 engine(6);
    const auto a = pxir_oracle::generate_f32(engine, n);
    const auto b = pxir_oracle::generate_f32(engine, n);
    std::vector<float> c(n);
    std::vector<float> d(n);
    pxir_oracle::native_add(a, b, c);
    pxir_oracle::native_add(c, b, d);

    pxir::Program p;
    const auto va = p.input(pxir::f32, n);
    const auto vb = p.input(pxir::f32, n);
    const auto vc = p.add(va, vb);
    p.output(vc);                // copy: vc is read again
    p.output(p.add(vc, vb));     // moved
    p.output(vc);                // move: last use
    auto verified = pxir::verify(std::move(p));
    if (!PXIR_CHECK(verified.ok())) return;
    const std::vector<Buffer> inputs{Buffer(a), Buffer(b)};
    const auto r = pxir::execute_cpu_reference(*verified.program, inputs);
    if (!PXIR_CHECK(r.ok()) || !PXIR_CHECK(r.outputs.size() == 3)) return;
    PXIR_CHECK(pxir_oracle::exactly_equal(*r.outputs[0].as_f32(), c));
    PXIR_CHECK(pxir_oracle::exactly_equal(*r.outputs[1].as_f32(), d));
    PXIR_CHECK(pxir_oracle::exactly_equal(*r.outputs[2].as_f32(), c));
    PXIR_CHECK(pxir_oracle::exactly_equal(*inputs[0].as_f32(), a));
    PXIR_CHECK(pxir_oracle::exactly_equal(*inputs[1].as_f32(), b));
}

}  // namespace

int main() {
    f32_lengths_around_vector_widths();
    f32_special_values_at_every_position();
    i32_wrap_boundaries();
    chained_add();
    m2_output_rules_still_hold();
    return pxir_test::finish("single_write");
}

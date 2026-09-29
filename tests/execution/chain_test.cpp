// M5: two dependent adds through the canonical executor. D = A + B; E = D + C,
// checked against the independent oracle at lengths around vector widths, for
// the final-only chain, the chain that outputs D before its later use (copy),
// and the chain that outputs D after its use (move); plus i32 wrap-around.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "check.hpp"
#include "pxir/runtime/cpu_reference.hpp"
#include "pxir/verify/verifier.hpp"
#include "pxir_oracle/oracle.hpp"

using pxir::Buffer;

namespace {

enum class Shape { final_only, intermediate_output, output_after_use };

pxir::VerifiedProgram chain(pxir::ScalarType t, std::uint32_t n, Shape shape) {
    pxir::Program p;
    const auto a = p.input(t, n);
    const auto b = p.input(t, n);
    const auto c = p.input(t, n);
    const auto d = p.add(a, b);
    if (shape == Shape::intermediate_output) p.output(d);
    const auto e = p.add(d, c);
    if (shape == Shape::output_after_use) p.output(d);
    p.output(e);
    pxir::VerifyResult r = pxir::verify(std::move(p));
    return std::move(*r.program);
}

bool same_i32(std::span<const std::int32_t> x, const std::vector<std::int32_t>& y) {
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < y.size(); ++i) {
        if (x[i] != y[i]) return false;
    }
    return true;
}

// Checks the outputs of a chain: [E] for final-only, [D, E] otherwise.
bool f32_outputs_match(const pxir::ExecutionResult& r, Shape shape, const std::vector<float>& d,
                       const std::vector<float>& e) {
    if (!r.ok()) return false;
    const std::size_t count = shape == Shape::final_only ? 1 : 2;
    if (r.outputs.size() != count) return false;
    if (count == 2 && !(r.outputs[0].f32_view() && pxir_oracle::exactly_equal(*r.outputs[0].f32_view(), d))) {
        return false;
    }
    const auto last = r.outputs.back().f32_view();
    return last && pxir_oracle::exactly_equal(*last, e);
}

void f32_chain_edge_lengths() {
    for (const std::uint32_t n : {1u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 33u, 4099u}) {
        std::mt19937_64 engine(700 + n);
        const auto a = pxir_oracle::generate_f32(engine, n);
        const auto b = pxir_oracle::generate_f32(engine, n);
        const auto c = pxir_oracle::generate_f32(engine, n);
        std::vector<float> d(n);
        std::vector<float> e(n);
        pxir_oracle::native_add(a, b, d);
        pxir_oracle::native_add(d, c, e);
        const std::vector<Buffer> inputs{Buffer(a), Buffer(b), Buffer(c)};
        for (const Shape s : {Shape::final_only, Shape::intermediate_output, Shape::output_after_use}) {
            const auto r = pxir::execute_cpu_reference(chain(pxir::f32, n, s), inputs);
            if (!PXIR_CHECK(f32_outputs_match(r, s, d, e))) std::fprintf(stderr, "  f32 n=%u shape=%d\n", n, int(s));
        }
        // Inputs are borrowed and unchanged.
        PXIR_CHECK(pxir_oracle::exactly_equal(*inputs[0].f32_view(), a));
        PXIR_CHECK(pxir_oracle::exactly_equal(*inputs[2].f32_view(), c));
    }
}

// Evaluation order is (A + B) + C: with A = 1, B = 2^-24, C = 2^-24 the two
// small terms are each lost to rounding, while A + (B + C) would give
// 1 + 2^-23.
void f32_evaluation_order() {
    const std::vector<float> a{1.0f};
    const std::vector<float> b{0x1p-24f};
    const std::vector<float> c{0x1p-24f};
    const std::vector<Buffer> inputs{Buffer(a), Buffer(b), Buffer(c)};
    const auto r = pxir::execute_cpu_reference(chain(pxir::f32, 1, Shape::final_only), inputs);
    if (!PXIR_CHECK(r.ok())) return;
    PXIR_CHECK((*r.outputs[0].f32_view())[0] == 1.0f);
    PXIR_CHECK((1.0f + (0x1p-24f + 0x1p-24f)) == 1.0f + 0x1p-23f);  // the other order differs
}

void f32_special_values() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<float> a{-0.0f, inf, 1.0f, nan, -inf};
    const std::vector<float> b{-0.0f, 1.0f, -inf, 1.0f, 5.0f};
    const std::vector<float> c{-0.0f, -inf, 2.0f, 0.0f, inf};
    std::vector<float> d(5);
    std::vector<float> e(5);
    pxir_oracle::native_add(a, b, d);
    pxir_oracle::native_add(d, c, e);
    const std::vector<Buffer> inputs{Buffer(a), Buffer(b), Buffer(c)};
    const auto r = pxir::execute_cpu_reference(chain(pxir::f32, 5, Shape::intermediate_output), inputs);
    PXIR_CHECK(f32_outputs_match(r, Shape::intermediate_output, d, e));
    if (r.ok()) PXIR_CHECK(std::signbit((*r.outputs[1].f32_view())[0]));  // -0 + -0 + -0 = -0
}

void i32_chain_wraps() {
    constexpr std::int32_t hi = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t lo = std::numeric_limits<std::int32_t>::min();
    const std::vector<std::int32_t> a{hi, lo, hi, -1, 0};
    const std::vector<std::int32_t> b{1, -1, hi, 1, lo};
    const std::vector<std::int32_t> c{hi, lo, 2, lo, lo};
    // D = {lo, hi, -2, 0, lo}; E = D + C = {-1, -1, 0, lo, 0}.
    const std::vector<std::int32_t> expected_d{lo, hi, -2, 0, lo};
    const std::vector<std::int32_t> expected_e{-1, -1, 0, lo, 0};
    std::vector<std::int32_t> od(5);
    std::vector<std::int32_t> oe(5);
    pxir_oracle::native_add(a, b, od);
    pxir_oracle::native_add(od, c, oe);
    PXIR_CHECK(od == expected_d && oe == expected_e);  // the oracle agrees with the hand values

    const std::vector<Buffer> inputs{Buffer(a), Buffer(b), Buffer(c)};
    const auto r = pxir::execute_cpu_reference(chain(pxir::i32, 5, Shape::intermediate_output), inputs);
    if (!PXIR_CHECK(r.ok() && r.outputs.size() == 2)) return;
    PXIR_CHECK(r.outputs[0].i32_view() && same_i32(*r.outputs[0].i32_view(), expected_d));
    PXIR_CHECK(r.outputs[1].i32_view() && same_i32(*r.outputs[1].i32_view(), expected_e));

    for (const std::uint32_t n : {1u, 5u, 8u, 17u, 4099u}) {
        std::mt19937_64 engine(800 + n);
        const auto x = pxir_oracle::generate_i32(engine, n);
        const auto y = pxir_oracle::generate_i32(engine, n);
        const auto z = pxir_oracle::generate_i32(engine, n);
        std::vector<std::int32_t> d(n);
        std::vector<std::int32_t> e(n);
        pxir_oracle::native_add(x, y, d);
        pxir_oracle::native_add(d, z, e);
        const std::vector<Buffer> in{Buffer(x), Buffer(y), Buffer(z)};
        const auto rr = pxir::execute_cpu_reference(chain(pxir::i32, n, Shape::final_only), in);
        PXIR_CHECK(rr.ok() && rr.outputs.size() == 1 && same_i32(*rr.outputs[0].i32_view(), e));
    }
}

}  // namespace

int main() {
    f32_chain_edge_lengths();
    f32_evaluation_order();
    f32_special_values();
    i32_chain_wraps();
    return pxir_test::finish("chain");
}

// Reference executor: results compared against the independent native oracle.

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "check.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"

using lume::Buffer;
using lume::ExecutionErrorCode;

namespace {

template <class T>
std::vector<T> to_vector(std::span<const T> s) {
    return std::vector<T>(s.begin(), s.end());
}

lume::VerifiedProgram vector_add(lume::ScalarType scalar, std::uint32_t n) {
    lume::Program p;
    const auto a = p.input(scalar, n);
    const auto b = p.input(scalar, n);
    p.output(p.add(a, b));
    lume::VerifyResult r = lume::verify(std::move(p));
    return std::move(*r.program);
}

void f32_add_matches_oracle(std::uint32_t n, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    const std::vector<float> a = lume_oracle::generate_f32(engine, n);
    const std::vector<float> b = lume_oracle::generate_f32(engine, n);
    std::vector<float> expected(n);
    lume_oracle::native_add(a, b, expected);

    const std::vector<Buffer> inputs{Buffer(a), Buffer(b)};
    const lume::ExecutionResult r = lume::execute_cpu_reference(vector_add(lume::f32, n), inputs);
    if (!LUME_CHECK(r.ok()) || !LUME_CHECK(r.outputs.size() == 1)) return;
    const auto c = r.outputs[0].f32_view();
    if (!LUME_CHECK(c.has_value())) return;
    LUME_CHECK(lume_oracle::exactly_equal(*c, expected));

    // Inputs are borrowed, never modified.
    LUME_CHECK(lume_oracle::exactly_equal(*inputs[0].f32_view(), a));
    LUME_CHECK(lume_oracle::exactly_equal(*inputs[1].f32_view(), b));
}

void f32_special_values_match_oracle() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float max = std::numeric_limits<float>::max();
    constexpr float denorm = std::numeric_limits<float>::denorm_min();
    const std::vector<float> a{0.0f, -0.0f, -0.0f, inf, max, denorm, 1.0f, 0.1f};
    const std::vector<float> b{-0.0f, -0.0f, 0.0f, -inf, max, denorm, 0x1p-24f, 0.2f};
    std::vector<float> expected(a.size());
    lume_oracle::native_add(a, b, expected);

    const std::vector<Buffer> inputs{Buffer(a), Buffer(b)};
    const auto r = lume::execute_cpu_reference(vector_add(lume::f32, static_cast<std::uint32_t>(a.size())), inputs);
    if (!LUME_CHECK(r.ok())) return;
    const std::span<const float> c = *r.outputs[0].f32_view();
    LUME_CHECK(lume_oracle::exactly_equal(c, expected));
    LUME_CHECK(std::signbit(c[1]));          // -0 + -0 = -0
    LUME_CHECK(!std::signbit(c[0]));         // +0 + -0 = +0 (round to nearest)
    LUME_CHECK(c[3] != c[3]);                // inf + -inf = NaN
    LUME_CHECK(c[4] == inf);                 // overflow to +inf
    LUME_CHECK(c[6] == 1.0f);                // ties-to-even
}

// The comparison policy itself: exact for non-NaN, any-NaN for NaN.
void comparison_policy_is_exact() {
    const float quiet = std::numeric_limits<float>::quiet_NaN();
    LUME_CHECK(lume_oracle::same_value(quiet, -quiet));
    LUME_CHECK(!lume_oracle::same_value(quiet, 0.0f));
    LUME_CHECK(!lume_oracle::same_value(0.0f, quiet));
    LUME_CHECK(!lume_oracle::same_value(0.0f, -0.0f));
    LUME_CHECK(!lume_oracle::same_value(1.0f, std::nextafter(1.0f, 2.0f)));
    LUME_CHECK(lume_oracle::same_value(0.3f, 0.3f));
}

void i32_add_wraps_and_matches_oracle() {
    constexpr std::int32_t hi = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t lo = std::numeric_limits<std::int32_t>::min();
    const std::vector<std::int32_t> a{hi, lo, -1, 7, hi};
    const std::vector<std::int32_t> b{1, -1, 1, -9, hi};
    const std::vector<Buffer> inputs{Buffer(a), Buffer(b)};
    const auto r = lume::execute_cpu_reference(vector_add(lume::i32, 5), inputs);
    if (!LUME_CHECK(r.ok())) return;
    const std::span<const std::int32_t> c = *r.outputs[0].i32_view();
    LUME_CHECK((to_vector(c) == std::vector<std::int32_t>{lo, hi, 0, -2, -2}));

    std::mt19937_64 engine(7);
    const auto x = lume_oracle::generate_i32(engine, 4096);
    const auto y = lume_oracle::generate_i32(engine, 4096);
    std::vector<std::int32_t> expected(4096);
    lume_oracle::native_add(x, y, expected);
    const std::vector<Buffer> random_inputs{Buffer(x), Buffer(y)};
    const auto rr = lume::execute_cpu_reference(vector_add(lume::i32, 4096), random_inputs);
    if (LUME_CHECK(rr.ok())) LUME_CHECK(to_vector(*rr.outputs[0].i32_view()) == expected);
}

void chained_adds_and_outputs_in_order() {
    lume::Program p;
    const auto a = p.input(lume::f32, 3);
    const auto b = p.input(lume::f32, 3);
    const auto c = p.add(a, b);
    const auto d = p.add(c, c);
    p.output(d);
    p.output(a);
    p.output(d);
    auto verified = lume::verify(std::move(p));
    if (!LUME_CHECK(verified.ok())) return;

    const std::vector<Buffer> inputs{Buffer(std::vector<float>{1, 2, 3}), Buffer(std::vector<float>{10, 20, 30})};
    const auto r = lume::execute_cpu_reference(*verified.program, inputs);
    if (!LUME_CHECK(r.ok()) || !LUME_CHECK(r.outputs.size() == 3)) return;
    LUME_CHECK((to_vector(*r.outputs[0].f32_view()) == std::vector<float>{22, 44, 66}));
    LUME_CHECK((to_vector(*r.outputs[1].f32_view()) == std::vector<float>{1, 2, 3}));
    LUME_CHECK((to_vector(*r.outputs[2].f32_view()) == std::vector<float>{22, 44, 66}));
}

// ---- Runtime buffers are validated against the IR (fail closed) -------------

void expect_error(const lume::ExecutionResult& r, ExecutionErrorCode code) {
    LUME_CHECK(!r.ok());
    LUME_CHECK(r.outputs.empty());
    if (r.error) LUME_CHECK(r.error->code == code);
}

void rejects_wrong_input_count() {
    const auto program = vector_add(lume::f32, 2);
    const std::vector<Buffer> one{Buffer(std::vector<float>{1, 2})};
    expect_error(lume::execute_cpu_reference(program, one), ExecutionErrorCode::input_count_mismatch);
    const std::vector<Buffer> three(3, Buffer(std::vector<float>{1, 2}));
    expect_error(lume::execute_cpu_reference(program, three), ExecutionErrorCode::input_count_mismatch);
    expect_error(lume::execute_cpu_reference(program, {}), ExecutionErrorCode::input_count_mismatch);
    // Count is reported before per-buffer problems.
    const std::vector<Buffer> wrong_typed{Buffer(std::vector<std::int32_t>{1, 2})};
    expect_error(lume::execute_cpu_reference(program, wrong_typed), ExecutionErrorCode::input_count_mismatch);
}

void rejects_wrong_input_scalar() {
    const std::vector<Buffer> inputs{Buffer(std::vector<float>{1, 2}), Buffer(std::vector<std::int32_t>{1, 2})};
    expect_error(lume::execute_cpu_reference(vector_add(lume::f32, 2), inputs),
                 ExecutionErrorCode::input_scalar_mismatch);
}

void rejects_wrong_input_length() {
    const std::vector<Buffer> inputs{Buffer(std::vector<float>{1, 2}), Buffer(std::vector<float>{1, 2, 3})};
    const auto r = lume::execute_cpu_reference(vector_add(lume::f32, 2), inputs);
    expect_error(r, ExecutionErrorCode::input_length_mismatch);
    if (r.error) LUME_CHECK(r.error->message == "input 1 (%1): expected length 2, got 3");
}

}  // namespace

int main() {
    f32_add_matches_oracle(1, 1);
    f32_add_matches_oracle(1024, 42);
    f32_add_matches_oracle(4099, 12345);  // not a multiple of any vector width
    f32_special_values_match_oracle();
    comparison_policy_is_exact();
    i32_add_wraps_and_matches_oracle();
    chained_adds_and_outputs_in_order();
    rejects_wrong_input_count();
    rejects_wrong_input_scalar();
    rejects_wrong_input_length();
    return lume_test::finish("execution");
}

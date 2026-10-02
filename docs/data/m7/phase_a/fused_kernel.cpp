// Lume M7 Phase A: isolated single-use-intermediate fusion kernels,
// independent of the Lume runtime (only the independent oracle is used).
//
//   left consumer:   t = a[i] + b[i]; r[i] = t + c[i]      ((A + B) + C)
//   right consumer:  t = a[i] + b[i]; r[i] = c[i] + t      (C + (A + B))
//
// i32 uses a wrapping helper twice: wrap(wrap(a + b) + c).
// The expected values always come from the oracle applied twice with a
// materialized intermediate, never from the fused kernels.
//
// Build (repository root):
//   g++|clang++ -std=c++20 -O3 -Ioracle/include
//       docs/data/m7/phase_a/fused_kernel.cpp -o fused_kernel

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include "lume_oracle/oracle.hpp"

static_assert(FLT_EVAL_METHOD == 0, "Phase A assumes no excess precision for float");

namespace {

std::int32_t wrapping_add(std::int32_t a, std::int32_t b) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

// Out-of-line so each kernel's code can be inspected on its own.
__attribute__((noinline)) void fused_left_f32(std::span<const float> a, std::span<const float> b,
                                              std::span<const float> c, std::span<float> r) {
    for (std::size_t i = 0; i < r.size(); ++i) {
        const float t = a[i] + b[i];
        r[i] = t + c[i];
    }
}

__attribute__((noinline)) void fused_right_f32(std::span<const float> a, std::span<const float> b,
                                               std::span<const float> c, std::span<float> r) {
    for (std::size_t i = 0; i < r.size(); ++i) {
        const float t = a[i] + b[i];
        r[i] = c[i] + t;
    }
}

__attribute__((noinline)) void fused_left_i32(std::span<const std::int32_t> a, std::span<const std::int32_t> b,
                                              std::span<const std::int32_t> c, std::span<std::int32_t> r) {
    for (std::size_t i = 0; i < r.size(); ++i) {
        const std::int32_t t = wrapping_add(a[i], b[i]);
        r[i] = wrapping_add(t, c[i]);
    }
}

__attribute__((noinline)) void fused_right_i32(std::span<const std::int32_t> a, std::span<const std::int32_t> b,
                                               std::span<const std::int32_t> c, std::span<std::int32_t> r) {
    for (std::size_t i = 0; i < r.size(); ++i) {
        const std::int32_t t = wrapping_add(a[i], b[i]);
        r[i] = wrapping_add(c[i], t);
    }
}

int failures = 0;
int checks = 0;
void check(bool ok, const char* what, std::size_t n) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s n=%zu\n", what, n);
    }
}

// Oracle: materialized D = A + B, then E = D + C (left) or E = C + D (right).
std::vector<float> oracle_f32(const std::vector<float>& a, const std::vector<float>& b, const std::vector<float>& c,
                              bool right) {
    std::vector<float> d(a.size()), e(a.size());
    lume_oracle::native_add(a, b, d);
    if (right) {
        lume_oracle::native_add(c, d, e);
    } else {
        lume_oracle::native_add(d, c, e);
    }
    return e;
}

std::vector<std::int32_t> oracle_i32(const std::vector<std::int32_t>& a, const std::vector<std::int32_t>& b,
                                     const std::vector<std::int32_t>& c, bool right) {
    std::vector<std::int32_t> d(a.size()), e(a.size());
    lume_oracle::native_add(a, b, d);
    if (right) {
        lume_oracle::native_add(c, d, e);
    } else {
        lume_oracle::native_add(d, c, e);
    }
    return e;
}

void f32_case(const std::vector<float>& a, const std::vector<float>& b, const std::vector<float>& c,
              const char* what) {
    const std::size_t n = a.size();
    std::vector<float> r(n);
    fused_left_f32(a, b, c, r);
    check(lume_oracle::exactly_equal(r, oracle_f32(a, b, c, false)), what, n);
    fused_right_f32(a, b, c, r);
    check(lume_oracle::exactly_equal(r, oracle_f32(a, b, c, true)), what, n);
}

void i32_case(const std::vector<std::int32_t>& a, const std::vector<std::int32_t>& b,
              const std::vector<std::int32_t>& c, const char* what) {
    const std::size_t n = a.size();
    std::vector<std::int32_t> r(n);
    fused_left_i32(a, b, c, r);
    check(r == oracle_i32(a, b, c, false), what, n);
    fused_right_i32(a, b, c, r);
    check(r == oracle_i32(a, b, c, true), what, n);
}

float bits(std::uint32_t u) { return std::bit_cast<float>(u); }

// Finite f32 values with random sign, a 24-bit significand and a binary
// exponent in [-20, 20]: sums of three such values usually need rounding,
// unlike generate_f32's values, which are multiples of 2^-23 in [-1, 1).
std::vector<float> rounding_sensitive(std::mt19937_64& engine, std::size_t n) {
    std::vector<float> out(n);
    for (float& x : out) {
        const std::uint64_t r = engine();
        const auto significand = static_cast<float>((r & 0xffffffu) | 0x800000u);  // [2^23, 2^24)
        const int exponent = static_cast<int>((r >> 24) % 41u) - 20 - 23;
        x = std::ldexp((r >> 63) != 0 ? -significand : significand, exponent);
    }
    return out;
}

}  // namespace

int main() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float den = std::numeric_limits<float>::denorm_min();
    constexpr float fmin = std::numeric_limits<float>::min();
    constexpr float fmax = std::numeric_limits<float>::max();

    // Edge lengths: random finite f32 and random i32.
    for (const std::uint32_t n : {1u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 33u, 4099u}) {
        std::mt19937_64 engine(7000 + n);
        const auto a = lume_oracle::generate_f32(engine, n);
        const auto b = lume_oracle::generate_f32(engine, n);
        const auto c = lume_oracle::generate_f32(engine, n);
        f32_case(a, b, c, "f32 random finite");
        const auto x = lume_oracle::generate_i32(engine, n);
        const auto y = lume_oracle::generate_i32(engine, n);
        const auto z = lume_oracle::generate_i32(engine, n);
        i32_case(x, y, z, "i32 random");
    }

    // f32 specials: signed zero, infinities, NaNs with different payloads
    // (quiet and signaling, both signs), subnormals, overflow to infinity.
    {
        const float qnan1 = bits(0x7fc00001u), qnan2 = bits(0xffc00002u), snan = bits(0x7f800001u);
        const std::vector<float> a{-0.0f, -0.0f, 0.0f, inf, inf, -inf, qnan1, 1.0f, snan, qnan1, den, -den, fmin, fmax, -fmax, den};
        const std::vector<float> b{-0.0f, 0.0f, -0.0f, -inf, 1.0f, -inf, 1.0f, qnan2, 2.0f, qnan2, den, den, -fmin, fmax, 1.0f, -den};
        const std::vector<float> c{-0.0f, -0.0f, -0.0f, 0.0f, -inf, inf, 3.0f, 0.0f, 1.0f, snan, -den, -den, fmin, -fmax, fmax, den};
        f32_case(a, b, c, "f32 specials");
        // Signed zero survives both shapes: (-0 + -0) + -0 = -0.
        std::vector<float> r(a.size());
        fused_left_f32(a, b, c, r);
        check(std::signbit(r[0]) && r[0] == 0.0f, "signed zero left", 1);
        fused_right_f32(a, b, c, r);
        check(std::signbit(r[0]) && r[0] == 0.0f, "signed zero right", 1);
    }

    // Rounding-order sentinels: (A + B) + C differs from the reassociated forms.
    //  s1: A = 1, B = 2^-24, C = 2^-24: (A+B)+C = 1, A+(B+C) = 1 + 2^-23.
    //  s2: A = 2^-24, B = 2^-24, C = 1: (A+B)+C = 1 + 2^-23, A+(B+C) = 1,
    //      and for the right shape C+(A+B) = 1 + 2^-23 while (C+A)+B = 1.
    {
        const std::vector<float> a{1.0f, 0x1p-24f};
        const std::vector<float> b{0x1p-24f, 0x1p-24f};
        const std::vector<float> c{0x1p-24f, 1.0f};
        check((1.0f + (0x1p-24f + 0x1p-24f)) != 1.0f, "sentinel s1 reassociated differs", 0);
        std::vector<float> r(2);
        fused_left_f32(a, b, c, r);
        check(r[0] == 1.0f && r[1] == 1.0f + 0x1p-23f, "rounding sentinel left", 2);
        fused_right_f32(a, b, c, r);
        check(r[0] == 1.0f && r[1] == 1.0f + 0x1p-23f, "rounding sentinel right", 2);
        f32_case(a, b, c, "rounding sentinel vs oracle");
    }

    // Rounding-sensitive random data. The fraction of elements where the
    // reassociated grouping differs is reported and must be non-zero, so these
    // checks are able to detect a grouping change.
    {
        std::mt19937_64 engine(424242);
        const std::size_t n = 4099;
        const auto a = rounding_sensitive(engine, n);
        const auto b = rounding_sensitive(engine, n);
        const auto c = rounding_sensitive(engine, n);
        std::size_t sensitive_left = 0, sensitive_right = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const float t = a[i] + b[i];
            if (std::bit_cast<std::uint32_t>(t + c[i]) != std::bit_cast<std::uint32_t>(a[i] + (b[i] + c[i]))) ++sensitive_left;
            if (std::bit_cast<std::uint32_t>(c[i] + t) != std::bit_cast<std::uint32_t>((c[i] + a[i]) + b[i])) ++sensitive_right;
        }
        std::printf("rounding_sensitive n=%zu left_grouping_sensitive=%zu right_grouping_sensitive=%zu\n", n, sensitive_left,
                    sensitive_right);
        check(sensitive_left > 0 && sensitive_right > 0, "rounding-sensitive data is sensitive", n);
        f32_case(a, b, c, "f32 rounding-sensitive random");
    }

    // i32 wrap boundaries, overflow, underflow and chained wrapping.
    {
        constexpr std::int32_t hi = std::numeric_limits<std::int32_t>::max();
        constexpr std::int32_t lo = std::numeric_limits<std::int32_t>::min();
        const std::vector<std::int32_t> a{hi, lo, hi, -1, 0, 1, hi, lo, -1, lo};
        const std::vector<std::int32_t> b{1, -1, hi, 1, lo, -1, hi, lo, -1, 0};
        const std::vector<std::int32_t> c{hi, lo, 2, lo, lo, 0, hi, lo, 1, -1};
        i32_case(a, b, c, "i32 boundaries");
        std::vector<std::int32_t> r(a.size());
        fused_left_i32(a, b, c, r);
        // Hand-computed: wrap(wrap(a+b)+c).
        const std::vector<std::int32_t> expect{-1, -1, 0, lo, 0, 0, hi - 2, lo, -1, hi};
        check(r == expect, "i32 hand-computed chained wrap", a.size());
    }

    std::printf("phase_a: %s (%d checks, %d failures)\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}

// M6 Phase A: isolated in-place add kernels, independent of the PXIR runtime.
// Build (from the repository root):
//   g++/clang++ -std=c++20 -O3 -Ioracle/include -Iinclude
//       docs/data/m6/phase_a/inplace_kernel.cpp -o inplace_kernel
// Only OwnedArray (M4) and the independent oracle are used.

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include "pxir/runtime/owned_array.hpp"
#include "pxir_oracle/oracle.hpp"

namespace {

std::int32_t wrapping_add(std::int32_t a, std::int32_t b) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

// Out-of-line so the compiler output can be inspected per kernel.
__attribute__((noinline)) void add_in_place_f32(std::span<float> destination, std::span<const float> other) {
    for (std::size_t i = 0; i < destination.size(); ++i) {
        destination[i] = destination[i] + other[i];
    }
}

__attribute__((noinline)) void add_in_place_i32(std::span<std::int32_t> destination,
                                                std::span<const std::int32_t> other) {
    for (std::size_t i = 0; i < destination.size(); ++i) {
        destination[i] = wrapping_add(destination[i], other[i]);
    }
}

int failures = 0;
void check(bool ok, const char* what, std::size_t n) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s n=%zu\n", what, n);
    }
}

template <class T>
pxir::OwnedArray<T> own(std::span<const T> src) {
    auto a = pxir::OwnedArray<T>::for_overwrite(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) a[i] = src[i];
    return a;
}

void f32_case(const std::vector<float>& a, const std::vector<float>& b, const std::vector<float>& c, const char* what) {
    const std::size_t n = a.size();
    std::vector<float> d(n), e(n);
    pxir_oracle::native_add(a, b, d);
    pxir_oracle::native_add(d, c, e);
    // D = A + B written into an OwnedArray, then D = D + C in place.
    auto dd = pxir::OwnedArray<float>::for_overwrite(n);
    for (std::size_t i = 0; i < n; ++i) dd[i] = a[i] + b[i];
    check(pxir_oracle::exactly_equal(dd.view(), d), what, n);
    add_in_place_f32({dd.data(), n}, c);
    check(pxir_oracle::exactly_equal(dd.view(), e), what, n);
}

}  // namespace

int main() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float den = std::numeric_limits<float>::denorm_min();
    for (const std::uint32_t n : {1u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 33u, 4099u}) {
        std::mt19937_64 engine(900 + n);
        const auto a = pxir_oracle::generate_f32(engine, n);
        const auto b = pxir_oracle::generate_f32(engine, n);
        const auto c = pxir_oracle::generate_f32(engine, n);
        f32_case(a, b, c, "f32 random");

        const auto x = pxir_oracle::generate_i32(engine, n);
        const auto y = pxir_oracle::generate_i32(engine, n);
        const auto z = pxir_oracle::generate_i32(engine, n);
        std::vector<std::int32_t> d(n), e(n);
        pxir_oracle::native_add(x, y, d);
        pxir_oracle::native_add(d, z, e);
        auto dd = own<std::int32_t>(d);
        add_in_place_i32({dd.data(), n}, z);
        bool same = true;
        for (std::size_t i = 0; i < n; ++i) same = same && dd[i] == e[i];
        check(same, "i32 random", n);
    }
    // f32 specials: signed zero, infinities, NaN, denormals.
    f32_case({-0.0f, inf, 1.0f, nan, -inf, den, -den, 0.0f}, {-0.0f, 1.0f, -inf, 1.0f, 5.0f, den, den, -0.0f},
             {-0.0f, -inf, 2.0f, 0.0f, inf, den, -den, -0.0f}, "f32 specials");
    {
        auto dd = own<float>(std::vector<float>{-0.0f});
        const std::vector<float> c{-0.0f};
        add_in_place_f32({dd.data(), 1}, c);
        check(std::signbit(dd[0]), "signed zero preserved", 1);
    }
    // i32 wrap boundaries.
    {
        constexpr std::int32_t hi = std::numeric_limits<std::int32_t>::max();
        constexpr std::int32_t lo = std::numeric_limits<std::int32_t>::min();
        auto dd = own<std::int32_t>(std::vector<std::int32_t>{hi, lo, hi, -1, 0});
        const std::vector<std::int32_t> c{1, -1, hi, 1, lo};
        add_in_place_i32({dd.data(), 5}, c);
        const std::int32_t expect[] = {lo, hi, -2, 0, lo};
        bool same = true;
        for (int i = 0; i < 5; ++i) same = same && dd[i] == expect[i];
        check(same, "i32 wrap boundaries", 5);
    }
    // Exact overlap (destination == other): d[i] = d[i] + d[i].
    {
        std::mt19937_64 engine(1);
        const auto a = pxir_oracle::generate_f32(engine, 4099);
        std::vector<float> expect(a.size());
        pxir_oracle::native_add(a, a, expect);
        auto dd = own<float>(a);
        add_in_place_f32({dd.data(), dd.size()}, dd.view());
        check(pxir_oracle::exactly_equal(dd.view(), expect), "same array twice f32", a.size());
    }
    std::printf("phase A: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

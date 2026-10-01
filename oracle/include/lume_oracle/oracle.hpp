#pragma once

// Correctness oracle for Lume M0.
//
// Deliberately independent of the Lume library: this header includes no Lume
// header and the lume_oracle target does not link lume. It provides
// deterministic input generation, native reference kernels, and exact
// comparison.

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace lume_oracle {

// Deterministic f32 values in [-1, 1) with 2^-23 granularity, all exactly
// representable. std::mt19937_64's output sequence is fixed by the C++
// standard, unlike the standard distributions, so the data is identical across
// compilers and platforms for a given seed.
inline std::vector<float> generate_f32(std::mt19937_64& engine, std::size_t count) {
    std::vector<float> out(count);
    for (float& x : out) {
        const auto bits24 = static_cast<std::uint32_t>(engine() >> 40);  // uniform in [0, 2^24)
        x = static_cast<float>(bits24) * 0x1p-23f - 1.0f;
    }
    return out;
}

inline std::vector<std::int32_t> generate_i32(std::mt19937_64& engine, std::size_t count) {
    std::vector<std::int32_t> out(count);
    for (std::int32_t& x : out) x = static_cast<std::int32_t>(static_cast<std::uint32_t>(engine() >> 32));
    return out;
}

// Native baseline: c[i] = a[i] + b[i]. Caller guarantees equal sizes.
inline void native_add(std::span<const float> a, std::span<const float> b, std::span<float> c) noexcept {
    for (std::size_t i = 0; i < c.size(); ++i) c[i] = a[i] + b[i];
}

// Native wrapping i32 add, written independently of the Lume executor.
inline void native_add(std::span<const std::int32_t> a, std::span<const std::int32_t> b,
                       std::span<std::int32_t> c) noexcept {
    for (std::size_t i = 0; i < c.size(); ++i) {
        const std::uint32_t sum = static_cast<std::uint32_t>(a[i]) + static_cast<std::uint32_t>(b[i]);
        c[i] = static_cast<std::int32_t>(sum);
    }
}

// Exact comparison policy. Every non-NaN result must match bit for bit
// (so -0.0 and +0.0 differ); there is no numeric tolerance. Both sides perform
// one correctly rounded binary32 addition on identical inputs, so any
// difference is a bug, not rounding noise.
//
// NaN is the single exception: a NaN matches any NaN. IEEE-754 does not
// specify the sign or payload of a NaN produced by an invalid operation, and
// they do differ in practice (x86 hardware yields 0xffc00000 for inf + -inf,
// while compile-time constant folding yields 0x7fc00000).
inline bool same_value(float x, float y) noexcept {
    if (std::isnan(x) || std::isnan(y)) return std::isnan(x) && std::isnan(y);
    return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
}

inline bool exactly_equal(std::span<const float> x, std::span<const float> y) noexcept {
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!same_value(x[i], y[i])) return false;
    }
    return true;
}

// FNV-1a over the bit patterns, for comparing results across runs.
inline std::uint64_t fnv1a(std::span<const float> values) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const float v : values) {
        std::uint32_t bits = std::bit_cast<std::uint32_t>(v);
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= bits & 0xffu;
            hash *= 0x100000001b3ull;
            bits >>= 8;
        }
    }
    return hash;
}

}  // namespace lume_oracle

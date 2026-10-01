#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace lume {

// Element types supported by M0. Zero is deliberately not a valid enumerator,
// so zero-initialized storage is detectably invalid.
enum class ScalarType : std::uint8_t {
    f32 = 1,  // IEEE-754 binary32
    i32 = 2,  // two's complement 32-bit; add wraps modulo 2^32
};

inline constexpr ScalarType f32 = ScalarType::f32;
inline constexpr ScalarType i32 = ScalarType::i32;

static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4,
              "Lume f32 requires IEEE-754 binary32 float");

[[nodiscard]] constexpr bool is_known(ScalarType scalar) noexcept {
    return scalar == ScalarType::f32 || scalar == ScalarType::i32;
}

// Returns 0 for an unknown scalar type.
[[nodiscard]] constexpr std::size_t scalar_size_bytes(ScalarType scalar) noexcept {
    switch (scalar) {
        case ScalarType::f32: return 4;
        case ScalarType::i32: return 4;
    }
    return 0;
}

[[nodiscard]] constexpr std::string_view to_string(ScalarType scalar) noexcept {
    switch (scalar) {
        case ScalarType::f32: return "f32";
        case ScalarType::i32: return "i32";
    }
    return "<invalid-scalar>";
}

// A fixed-length one-dimensional buffer type, e.g. f32[1024].
// M0 has no ranks, symbolic dimensions, or dynamic shapes.
struct Type {
    ScalarType scalar{};
    std::uint32_t length = 0;

    friend constexpr bool operator==(Type, Type) noexcept = default;
};

// Total bytes for one buffer of `type`; nullopt for an unknown scalar type or
// when the product does not fit in std::size_t.
[[nodiscard]] constexpr std::optional<std::size_t> byte_size(Type type) noexcept {
    const std::size_t element = scalar_size_bytes(type.scalar);
    if (element == 0) return std::nullopt;
    if (type.length > std::numeric_limits<std::size_t>::max() / element) return std::nullopt;
    return static_cast<std::size_t>(type.length) * element;
}

}  // namespace lume

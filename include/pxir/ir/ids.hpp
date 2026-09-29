#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pxir {

namespace detail {

// A 32-bit index into one of Program's contiguous tables.
//
// Each Tag produces a distinct type, so a ValueId cannot be passed where an
// OperationId or TypeId is expected. Construction from an integer is explicit.
// The all-ones value is reserved as the invalid sentinel, and a
// default-constructed id is invalid.
template <class Tag>
struct Id {
    static constexpr std::uint32_t invalid_value = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t value = invalid_value;

    constexpr Id() noexcept = default;
    constexpr explicit Id(std::uint32_t raw) noexcept : value(raw) {}

    [[nodiscard]] static constexpr Id invalid() noexcept { return Id{}; }
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != invalid_value; }
    [[nodiscard]] constexpr std::size_t index() const noexcept { return value; }

    friend constexpr bool operator==(Id, Id) noexcept = default;
};

struct ValueTag;
struct TypeTag;
struct OperationTag;

}  // namespace detail

using ValueId = detail::Id<detail::ValueTag>;
using TypeId = detail::Id<detail::TypeTag>;
using OperationId = detail::Id<detail::OperationTag>;

// Intentional invariants: ids are exactly one 32-bit word and trivially copyable.
static_assert(sizeof(ValueId) == sizeof(std::uint32_t));
static_assert(sizeof(TypeId) == sizeof(std::uint32_t));
static_assert(sizeof(OperationId) == sizeof(std::uint32_t));
static_assert(std::is_trivially_copyable_v<ValueId>);
static_assert(!std::is_convertible_v<ValueId, OperationId>);
static_assert(!std::is_convertible_v<std::uint32_t, ValueId>);

}  // namespace pxir

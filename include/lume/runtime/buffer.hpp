#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "lume/ir/types.hpp"
#include "lume/runtime/owned_array.hpp"

namespace lume {

namespace detail {
// Internal executor access to mutable storage; see Buffer. Not public API.
struct RuntimeBufferAccess;
}  // namespace detail

// A host buffer that owns typed elements. Used for executor inputs and outputs.
//
// Storage is either a caller-provided std::vector (moved in, never copied) or
// an OwnedArray produced by the executor. Both hold f32 or i32 elements and
// are read through the same span views; callers do not depend on which one a
// buffer uses. Copies are deep; moves never throw.
class Buffer {
public:
    explicit Buffer(std::vector<float> data) : data_(std::move(data)) {}
    explicit Buffer(std::vector<std::int32_t> data) : data_(std::move(data)) {}
    explicit Buffer(OwnedArray<float> data) noexcept : data_(std::move(data)) {}
    explicit Buffer(OwnedArray<std::int32_t> data) noexcept : data_(std::move(data)) {}

    [[nodiscard]] ScalarType scalar() const noexcept {
        return std::visit(
            [](const auto& v) {
                using T = typename std::decay_t<decltype(v)>::value_type;
                return std::is_same_v<T, float> ? ScalarType::f32 : ScalarType::i32;
            },
            data_);
    }

    [[nodiscard]] std::size_t length() const noexcept {
        return std::visit([](const auto& v) { return v.size(); }, data_);
    }

    // Read-only views of the elements; nullopt when the buffer holds the
    // other scalar type. No allocation, no copy.
    [[nodiscard]] std::optional<std::span<const float>> f32_view() const noexcept { return view<float>(); }
    [[nodiscard]] std::optional<std::span<const std::int32_t>> i32_view() const noexcept {
        return view<std::int32_t>();
    }

private:
    // The only route to mutable elements. The executor uses it to overwrite a
    // buffer it owns; the public interface stays read-only.
    friend struct detail::RuntimeBufferAccess;

    template <class T>
    [[nodiscard]] std::optional<std::span<const T>> view() const noexcept {
        if (const auto* v = std::get_if<std::vector<T>>(&data_)) return std::span<const T>(v->data(), v->size());
        if (const auto* a = std::get_if<OwnedArray<T>>(&data_)) return a->view();
        return std::nullopt;
    }

    std::variant<std::vector<float>, std::vector<std::int32_t>, OwnedArray<float>, OwnedArray<std::int32_t>> data_;
};

static_assert(std::is_nothrow_move_constructible_v<Buffer>);
static_assert(std::is_copy_constructible_v<Buffer>);

}  // namespace lume

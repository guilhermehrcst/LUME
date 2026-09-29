#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace pxir {

// A contiguous, owning, fixed-size array of f32 or i32 whose elements are NOT
// value-initialized on creation.
//
// Lifetime: for_overwrite(n) is new T[n] (via std::make_unique_for_overwrite),
// i.e. default-initialization. For these scalar types that starts the lifetime
// of all n elements and leaves their values indeterminate. Reading an element
// before writing it is undefined behavior, so a creator must assign every
// element before the array is read or handed to anyone else. Copies are deep,
// moves transfer ownership and leave the source empty (size 0).
template <class T>
class OwnedArray {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, std::int32_t>,
                  "OwnedArray is restricted to PXIR's scalar representations");
    static_assert(std::is_trivially_default_constructible_v<T>);
    static_assert(std::is_trivially_destructible_v<T>);
    static_assert(std::is_trivially_copyable_v<T>);

public:
    using value_type = T;

    OwnedArray() noexcept = default;

    // n elements with indeterminate values; see the class comment.
    [[nodiscard]] static OwnedArray for_overwrite(std::size_t n) {
#if defined(__cpp_lib_smart_ptr_for_overwrite)
        return OwnedArray(std::make_unique_for_overwrite<T[]>(n), n);
#else
        // The specified equivalent of make_unique_for_overwrite<T[]>(n).
        return OwnedArray(std::unique_ptr<T[]>(new T[n]), n);
#endif
    }

    OwnedArray(const OwnedArray& other) : OwnedArray(for_overwrite(other.size_)) {
        std::copy_n(other.data_.get(), other.size_, data_.get());
    }

    OwnedArray(OwnedArray&& other) noexcept
        : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0)) {}

    // Copy-and-swap: on allocation failure *this is unchanged.
    OwnedArray& operator=(const OwnedArray& other) {
        if (this != &other) {
            OwnedArray copy(other);
            swap(copy);
        }
        return *this;
    }

    OwnedArray& operator=(OwnedArray&& other) noexcept {
        data_ = std::move(other.data_);
        size_ = std::exchange(other.size_, 0);
        return *this;
    }

    ~OwnedArray() = default;

    void swap(OwnedArray& other) noexcept {
        data_.swap(other.data_);
        std::swap(size_, other.size_);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] T* data() noexcept { return data_.get(); }
    [[nodiscard]] const T* data() const noexcept { return data_.get(); }

    // Unchecked, like std::vector::operator[].
    [[nodiscard]] T& operator[](std::size_t i) noexcept { return data_[i]; }
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return data_[i]; }

    [[nodiscard]] std::span<const T> view() const noexcept { return {data_.get(), size_}; }

private:
    OwnedArray(std::unique_ptr<T[]> data, std::size_t size) noexcept : data_(std::move(data)), size_(size) {}

    std::unique_ptr<T[]> data_;
    std::size_t size_ = 0;
};

static_assert(std::is_nothrow_move_constructible_v<OwnedArray<float>>);
static_assert(std::is_nothrow_move_assignable_v<OwnedArray<float>>);
static_assert(std::is_copy_constructible_v<OwnedArray<float>>);

}  // namespace pxir

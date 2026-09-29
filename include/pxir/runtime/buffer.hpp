#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include "pxir/ir/types.hpp"

namespace pxir {

// A host buffer that owns typed elements. Used for executor inputs and outputs.
class Buffer {
public:
    explicit Buffer(std::vector<float> data) : data_(std::move(data)) {}
    explicit Buffer(std::vector<std::int32_t> data) : data_(std::move(data)) {}

    [[nodiscard]] ScalarType scalar() const noexcept {
        return std::holds_alternative<std::vector<float>>(data_) ? ScalarType::f32 : ScalarType::i32;
    }

    [[nodiscard]] std::size_t length() const noexcept {
        return std::visit([](const auto& v) { return v.size(); }, data_);
    }

    // nullptr when the buffer holds a different scalar type.
    [[nodiscard]] const std::vector<float>* as_f32() const noexcept { return std::get_if<std::vector<float>>(&data_); }
    [[nodiscard]] const std::vector<std::int32_t>* as_i32() const noexcept {
        return std::get_if<std::vector<std::int32_t>>(&data_);
    }

private:
    std::variant<std::vector<float>, std::vector<std::int32_t>> data_;
};

}  // namespace pxir

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pxir/runtime/buffer.hpp"
#include "pxir/verify/verifier.hpp"

namespace pxir {

enum class ExecutionErrorCode : std::uint8_t {
    input_count_mismatch,          // number of buffers != number of input operations
    input_scalar_mismatch,         // buffer scalar type != input value type
    input_length_mismatch,         // buffer length != input value length
    internal_invariant_violation,  // verified IR reached an impossible state (a PXIR bug)
};

[[nodiscard]] std::string_view to_string(ExecutionErrorCode code) noexcept;

struct ExecutionError {
    ExecutionErrorCode code;
    std::string message;
};

struct ExecutionResult {
    std::vector<Buffer> outputs;          // one per output operation, in program order; empty on error
    std::optional<ExecutionError> error;

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

// Scalar CPU reference executor. Interprets the verified IR operation by
// operation; correctness is the only goal.
//
// `inputs[k]` is bound to the k-th input operation. All inputs are validated
// against the IR types before any computation. Inputs are not copied; every
// add allocates a fresh result buffer, and every output copies its value.
//
// Semantics: f32 add is IEEE-754 binary32 addition in the current rounding
// mode; i32 add wraps modulo 2^32.
[[nodiscard]] ExecutionResult execute_cpu_reference(const VerifiedProgram& program, std::span<const Buffer> inputs);

}  // namespace pxir

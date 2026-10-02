#pragma once

// Internal to the lume library (not installed under include/). A test seam
// for M7: fusion of an add pair is not observable through the public API
// (same values, same allocation count), so tests use this entry point to see
// which pairs the real executor fused.

#include <cstdint>
#include <span>
#include <vector>

#include "lume/runtime/buffer.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"

namespace lume::detail {

// Identical to execute_cpu_reference. When `fused` is non-null, the index of
// the first operation of every add pair executed as one fused loop is
// appended to it, in execution order.
[[nodiscard]] ExecutionResult execute_cpu_reference_observed(const VerifiedProgram& program,
                                                             std::span<const Buffer> inputs,
                                                             std::vector<std::uint32_t>* fused);

}  // namespace lume::detail

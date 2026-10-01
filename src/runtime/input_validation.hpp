#pragma once

// Internal to the lume library (not installed under include/). Exposed to the
// executor-breakdown benchmark so it can time the executor's real input
// validation instead of a copy of it.

#include <optional>
#include <span>

#include "lume/ir/program.hpp"
#include "lume/runtime/buffer.hpp"
#include "lume/runtime/cpu_reference.hpp"

namespace lume::detail {

// Phase 1 of execute_cpu_reference: checks the buffer count, then each
// buffer's scalar type and length against its input operation's IR type.
// Returns the first error, or nullopt when every buffer matches.
// Precondition: `storage` belongs to a VerifiedProgram.
[[nodiscard]] std::optional<ExecutionError> validate_inputs(const ProgramStorage& storage,
                                                            std::span<const Buffer> inputs);

}  // namespace lume::detail

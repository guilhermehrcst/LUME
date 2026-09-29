#include "pxir/runtime/cpu_reference.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace pxir {

std::string_view to_string(ExecutionErrorCode code) noexcept {
    switch (code) {
        case ExecutionErrorCode::input_count_mismatch: return "input_count_mismatch";
        case ExecutionErrorCode::input_scalar_mismatch: return "input_scalar_mismatch";
        case ExecutionErrorCode::input_length_mismatch: return "input_length_mismatch";
        case ExecutionErrorCode::internal_invariant_violation: return "internal_invariant_violation";
    }
    return "<invalid-execution-error-code>";
}

namespace {

ExecutionResult fail(ExecutionErrorCode code, std::string message) {
    return ExecutionResult{{}, ExecutionError{code, std::move(message)}};
}

ExecutionResult internal_error(std::string message) {
    return fail(ExecutionErrorCode::internal_invariant_violation, "internal invariant violation: " + std::move(message));
}

// Wrapping i32 addition without signed-overflow UB: add as uint32_t, then
// convert back (modular conversion is well-defined since C++20).
std::int32_t wrapping_add(std::int32_t a, std::int32_t b) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

// The reference kernel. nullopt when the buffers disagree in scalar type or
// length, which a verified program with validated inputs never produces.
std::optional<Buffer> add_buffers(const Buffer& lhs, const Buffer& rhs) {
    if (const auto* a = lhs.as_f32()) {
        const auto* b = rhs.as_f32();
        if (b == nullptr || a->size() != b->size()) return std::nullopt;
        std::vector<float> c(a->size());
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = (*a)[i] + (*b)[i];
        return Buffer(std::move(c));
    }
    if (const auto* a = lhs.as_i32()) {
        const auto* b = rhs.as_i32();
        if (b == nullptr || a->size() != b->size()) return std::nullopt;
        std::vector<std::int32_t> c(a->size());
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = wrapping_add((*a)[i], (*b)[i]);
        return Buffer(std::move(c));
    }
    return std::nullopt;
}

}  // namespace

ExecutionResult execute_cpu_reference(const VerifiedProgram& verified, std::span<const Buffer> inputs) {
    const ProgramStorage& s = verified.program().storage();

    // Phase 1: validate every caller buffer against the IR before computing anything.
    std::size_t input_count = 0;
    for (const Operation& op : s.operations) {
        if (op.opcode == Opcode::input) ++input_count;
    }
    if (inputs.size() != input_count) {
        return fail(ExecutionErrorCode::input_count_mismatch, "program has " + std::to_string(input_count) +
                                                                  " inputs, got " + std::to_string(inputs.size()) +
                                                                  " buffers");
    }
    std::size_t k = 0;
    for (const Operation& op : s.operations) {
        if (op.opcode != Opcode::input) continue;
        const Buffer& buffer = inputs[k];
        const Type type = s.types[s.values[op.result.index()].type.index()];
        const std::string where = "input " + std::to_string(k) + " (%" + std::to_string(op.result.value) + ")";
        if (buffer.scalar() != type.scalar) {
            return fail(ExecutionErrorCode::input_scalar_mismatch, where + ": expected " +
                                                                       std::string(to_string(type.scalar)) + ", got " +
                                                                       std::string(to_string(buffer.scalar())));
        }
        if (buffer.length() != type.length) {
            return fail(ExecutionErrorCode::input_length_mismatch, where + ": expected length " +
                                                                       std::to_string(type.length) + ", got " +
                                                                       std::to_string(buffer.length()));
        }
        ++k;
    }

    // Phase 2: interpret operations in order. bound[v] points at the buffer
    // holding value v: a caller input or an entry of `owned`. Both vectors are
    // sized once and never resized, so the pointers stay valid.
    std::vector<const Buffer*> bound(s.values.size(), nullptr);
    std::vector<std::optional<Buffer>> owned(s.values.size());
    const auto lookup = [&](ValueId v) -> const Buffer* { return v.index() < bound.size() ? bound[v.index()] : nullptr; };

    ExecutionResult result;
    std::size_t next_input = 0;
    for (std::size_t i = 0; i < s.operations.size(); ++i) {
        const Operation& op = s.operations[i];
        switch (op.opcode) {
            case Opcode::input:
                bound[op.result.index()] = &inputs[next_input++];
                break;
            case Opcode::add: {
                const Buffer* lhs = lookup(op.operands[0]);
                const Buffer* rhs = lookup(op.operands[1]);
                if (lhs == nullptr || rhs == nullptr) return internal_error("op " + std::to_string(i) + " operand unbound");
                std::optional<Buffer> sum = add_buffers(*lhs, *rhs);
                if (!sum) return internal_error("op " + std::to_string(i) + " operand buffers disagree");
                owned[op.result.index()] = std::move(sum);
                bound[op.result.index()] = &*owned[op.result.index()];
                break;
            }
            case Opcode::output: {
                const Buffer* value = lookup(op.operands[0]);
                if (value == nullptr) return internal_error("op " + std::to_string(i) + " output unbound");
                result.outputs.push_back(*value);
                break;
            }
            default:
                return internal_error("op " + std::to_string(i) + " has unknown opcode");
        }
    }
    return result;
}

}  // namespace pxir

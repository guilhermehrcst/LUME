#include "pxir/runtime/cpu_reference.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "input_validation.hpp"

namespace pxir {

namespace detail {

// Mutable access to executor-owned storage. Only OwnedArray-backed buffers
// (created by the executor) expose mutable elements; a vector-backed buffer,
// which is how caller inputs are stored, yields nullopt for both types.
struct RuntimeBufferAccess {
    template <class T>
    [[nodiscard]] static std::optional<std::span<T>> mutable_elements(Buffer& buffer) noexcept {
        if (auto* array = std::get_if<OwnedArray<T>>(&buffer.data_)) return std::span<T>(array->data(), array->size());
        return std::nullopt;
    }
};

}  // namespace detail

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

// Transferring an owned Buffer into ExecutionResult::outputs relies on Buffer
// being nothrow move constructible; vector growth in `outputs` also relies on
// it to move rather than copy existing elements. After a move the source is
// valid but its value is unspecified: PXIR never inspects or reuses it, and
// the executor immediately resets the owned slot and clears bound[id].
static_assert(std::is_nothrow_move_constructible_v<Buffer>);

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
//
// Each result is an OwnedArray created without value-initialization; the loop
// then writes every element exactly once, and only the completed array is
// wrapped in a Buffer. No element is read before it is written.
std::optional<Buffer> add_buffers(const Buffer& lhs, const Buffer& rhs) {
    if (const auto a = lhs.f32_view()) {
        const auto b = rhs.f32_view();
        if (!b || a->size() != b->size()) return std::nullopt;
        const std::span<const float> x = *a;
        const std::span<const float> y = *b;
        auto c = OwnedArray<float>::for_overwrite(x.size());
        for (std::size_t i = 0; i < x.size(); ++i) c[i] = x[i] + y[i];
        return Buffer(std::move(c));
    }
    if (const auto a = lhs.i32_view()) {
        const auto b = rhs.i32_view();
        if (!b || a->size() != b->size()) return std::nullopt;
        const std::span<const std::int32_t> x = *a;
        const std::span<const std::int32_t> y = *b;
        auto c = OwnedArray<std::int32_t>::for_overwrite(x.size());
        for (std::size_t i = 0; i < x.size(); ++i) c[i] = wrapping_add(x[i], y[i]);
        return Buffer(std::move(c));
    }
    return std::nullopt;
}

// d[i] = lhs[i] + rhs[i] where the destination is the storage of `lhs`, of
// `rhs`, or of both (the same value used twice). Operand order follows the IR
// in the source; the compiler may still commute the addition, which can change
// only the payload of a NaN result.
// `other` is the non-destination operand; when both operands are the
// destination it is the destination itself. Each iteration reads its operands
// at i before writing element i, and never touches another index, so the
// exact overlap of destination and operand is safe.
enum class Reuse { lhs, rhs, both };

template <class T, class Add>
void add_in_place(std::span<T> destination, std::span<const T> other, Reuse reuse, Add add) noexcept {
    const std::size_t n = destination.size();
    T* d = destination.data();
    const T* o = other.data();
    switch (reuse) {
        case Reuse::lhs:
            for (std::size_t i = 0; i < n; ++i) d[i] = add(d[i], o[i]);
            break;
        case Reuse::rhs:
            for (std::size_t i = 0; i < n; ++i) d[i] = add(o[i], d[i]);
            break;
        case Reuse::both:
            for (std::size_t i = 0; i < n; ++i) d[i] = add(d[i], d[i]);
            break;
    }
}

// In-place counterpart of add_buffers. `destination` must be the very object
// that `lhs`, `rhs`, or both refer to, and must be executor-owned
// (OwnedArray-backed). Nothing is written unless scalar type and length agree
// across all three; false means no element was modified. No allocation.
bool add_buffers_in_place(Buffer& destination, const Buffer& lhs, const Buffer& rhs) {
    const bool is_lhs = &lhs == &destination;
    const bool is_rhs = &rhs == &destination;
    if (!is_lhs && !is_rhs) return false;
    const Reuse reuse = is_lhs && is_rhs ? Reuse::both : (is_lhs ? Reuse::lhs : Reuse::rhs);
    const Buffer& other = is_lhs ? rhs : lhs;  // the destination itself when both

    if (const auto o = other.f32_view()) {
        const auto d = detail::RuntimeBufferAccess::mutable_elements<float>(destination);
        if (!d || d->size() != o->size()) return false;
        add_in_place<float>(*d, *o, reuse, [](float a, float b) noexcept { return a + b; });
        return true;
    }
    if (const auto o = other.i32_view()) {
        const auto d = detail::RuntimeBufferAccess::mutable_elements<std::int32_t>(destination);
        if (!d || d->size() != o->size()) return false;
        add_in_place<std::int32_t>(*d, *o, reuse, wrapping_add);
        return true;
    }
    return false;
}

}  // namespace

namespace detail {

std::optional<ExecutionError> validate_inputs(const ProgramStorage& s, std::span<const Buffer> inputs) {
    std::size_t input_count = 0;
    for (const Operation& op : s.operations) {
        if (op.opcode == Opcode::input) ++input_count;
    }
    if (inputs.size() != input_count) {
        return ExecutionError{ExecutionErrorCode::input_count_mismatch,
                              "program has " + std::to_string(input_count) + " inputs, got " +
                                  std::to_string(inputs.size()) + " buffers"};
    }
    std::size_t k = 0;
    for (const Operation& op : s.operations) {
        if (op.opcode != Opcode::input) continue;
        const Buffer& buffer = inputs[k];
        const Type type = s.types[s.values[op.result.index()].type.index()];
        const std::string where = "input " + std::to_string(k) + " (%" + std::to_string(op.result.value) + ")";
        if (buffer.scalar() != type.scalar) {
            return ExecutionError{ExecutionErrorCode::input_scalar_mismatch,
                                  where + ": expected " + std::string(to_string(type.scalar)) + ", got " +
                                      std::string(to_string(buffer.scalar()))};
        }
        if (buffer.length() != type.length) {
            return ExecutionError{ExecutionErrorCode::input_length_mismatch,
                                  where + ": expected length " + std::to_string(type.length) + ", got " +
                                      std::to_string(buffer.length())};
        }
        ++k;
    }
    return std::nullopt;
}

}  // namespace detail

ExecutionResult execute_cpu_reference(const VerifiedProgram& verified, std::span<const Buffer> inputs) {
    const ProgramStorage& s = verified.program().storage();

    // Phase 1: validate every caller buffer against the IR before computing anything.
    if (std::optional<ExecutionError> error = detail::validate_inputs(s, inputs)) {
        return ExecutionResult{{}, std::move(*error)};
    }

    // Phase 2: interpret operations in order. bound[v] points at the buffer
    // holding value v: a caller input (borrowed) or an entry of `owned`
    // (executor-owned). Both vectors are sized once and never resized, so the
    // pointers stay valid.
    std::vector<const Buffer*> bound(s.values.size(), nullptr);
    std::vector<std::optional<Buffer>> owned(s.values.size());

    // last_use[v] is the index of the last operation that reads value v; the
    // invalid id means v is never read. The program is verified, so every
    // operand id is valid and defined before use.
    std::vector<OperationId> last_use(s.values.size());
    for (std::size_t i = 0; i < s.operations.size(); ++i) {
        for (const ValueId operand : s.operations[i].operands) {
            if (operand.is_valid()) last_use[operand.index()] = OperationId{static_cast<std::uint32_t>(i)};
        }
    }
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
                // An operand is reusable as the destination iff the executor owns
                // it and this add is its last use (outputs count as uses, so a
                // later output blocks reuse). Caller inputs are never owned.
                // Preference: lhs, then rhs.
                const OperationId here{static_cast<std::uint32_t>(i)};
                const auto reusable = [&](ValueId v) { return owned[v.index()].has_value() && last_use[v.index()] == here; };
                const ValueId reuse = reusable(op.operands[0]) ? op.operands[0]
                                      : reusable(op.operands[1]) ? op.operands[1]
                                                                 : ValueId{};
                if (reuse.is_valid()) {
                    // Compute inside the operand's storage first; only then
                    // move ownership to the result and unbind the old value.
                    if (!add_buffers_in_place(*owned[reuse.index()], *lhs, *rhs)) {
                        return internal_error("op " + std::to_string(i) + " operand buffers disagree");
                    }
                    owned[op.result.index()] = std::move(owned[reuse.index()]);
                    owned[reuse.index()].reset();
                    bound[reuse.index()] = nullptr;
                    bound[op.result.index()] = &*owned[op.result.index()];
                    break;
                }
                std::optional<Buffer> sum = add_buffers(*lhs, *rhs);
                if (!sum) return internal_error("op " + std::to_string(i) + " operand buffers disagree");
                owned[op.result.index()] = std::move(sum);
                bound[op.result.index()] = &*owned[op.result.index()];
                break;
            }
            case Opcode::output: {
                const ValueId id = op.operands[0];
                const Buffer* value = lookup(id);
                if (value == nullptr) return internal_error("op " + std::to_string(i) + " output unbound");
                std::optional<Buffer>& slot = owned[id.index()];
                if (slot.has_value() && last_use[id.index()] == OperationId{static_cast<std::uint32_t>(i)}) {
                    // Executor-owned and never read again: transfer the buffer
                    // instead of copying it, then unbind it so no later lookup
                    // can reach the moved-from state.
                    result.outputs.push_back(std::move(*slot));
                    slot.reset();
                    bound[id.index()] = nullptr;
                } else {
                    // Caller-owned input, or a value still read later: copy.
                    result.outputs.push_back(*value);
                }
                break;
            }
            default:
                return internal_error("op " + std::to_string(i) + " has unknown opcode");
        }
    }
    return result;
}

}  // namespace pxir

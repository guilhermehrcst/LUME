#include "lume/runtime/cpu_reference.hpp"

#include <cfloat>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "fusion_observer.hpp"
#include "input_validation.hpp"

namespace lume {

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
// valid but its value is unspecified: Lume never inspects or reuses it, and
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

// M7 fused kernel for an add pair whose intermediate t is used only by the
// second add: r[i] = t + c[i], or r[i] = c[i] + t when t is the right operand,
// with t = a[i] + b[i] a loop-local value. The grouping is (A + B) then the
// second add, exactly as in the IR; nothing is reassociated. As in
// add_in_place, the compiler may still commute an individual addition, which
// can change only the payload of a NaN result.
template <class T, class Add>
OwnedArray<T> add_add(std::span<const T> a, std::span<const T> b, std::span<const T> c, bool t_is_rhs, Add add) {
    auto r = OwnedArray<T>::for_overwrite(a.size());
    if (t_is_rhs) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            const T t = add(a[i], b[i]);
            r[i] = add(c[i], t);
        }
    } else {
        for (std::size_t i = 0; i < a.size(); ++i) {
            const T t = add(a[i], b[i]);
            r[i] = add(t, c[i]);
        }
    }
    return r;
}

// Fused counterpart of add_buffers: one result buffer, every element written
// exactly once, no buffer for the intermediate. nullopt (before allocating)
// when scalar types or lengths disagree.
std::optional<Buffer> add_add_buffers(const Buffer& a, const Buffer& b, const Buffer& c, bool t_is_rhs) {
    if (const auto x = a.f32_view()) {
        const auto y = b.f32_view();
        const auto z = c.f32_view();
        if (!y || !z || x->size() != y->size() || x->size() != z->size()) return std::nullopt;
        return Buffer(add_add<float>(*x, *y, *z, t_is_rhs, [](float u, float v) noexcept { return u + v; }));
    }
    if (const auto x = a.i32_view()) {
        const auto y = b.i32_view();
        const auto z = c.i32_view();
        if (!y || !z || x->size() != y->size() || x->size() != z->size()) return std::nullopt;
        return Buffer(add_add<std::int32_t>(*x, *y, *z, t_is_rhs, wrapping_add));
    }
    return std::nullopt;
}

// A loop-local float intermediate equals the rounded binary32 sum that M6
// stores in memory only when float expressions carry no excess precision.
// Where they might (e.g. x87), or where the toolchain does not say, f32 pairs
// are not fused.
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
constexpr bool f32_intermediate_is_exact = true;
#else
constexpr bool f32_intermediate_is_exact = false;
#endif

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
    return detail::execute_cpu_reference_observed(verified, inputs, nullptr);
}

ExecutionResult detail::execute_cpu_reference_observed(const VerifiedProgram& verified, std::span<const Buffer> inputs,
                                                       std::vector<std::uint32_t>* fused) {
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

    // M6 reuse rule: v can be the destination of the add at `at` iff the
    // executor owns it and that add is its last use (outputs count as uses,
    // so a later output blocks reuse). Caller inputs are never owned.
    const auto reusable = [&](ValueId v, std::size_t at) {
        return owned[v.index()].has_value() && last_use[v.index()] == OperationId{static_cast<std::uint32_t>(at)};
    };

    // M7: the add at k (T = A + B) and the add at k + 1 (R = T + C or
    // R = C + T) run as one loop, and T is never materialized, iff
    //   1. operation k + 1 exists and is an add (adjacent; no search),
    //   2. T is exactly one of its operands,
    //   3. last_use[T] == k + 1 (T is read by nothing else and not output),
    //   4. neither A nor B is M6-reusable at k,
    //   5. C is not M6-reusable at k + 1,
    //   6. T is i32, or f32 without excess precision.
    // 4 and 5 keep the pair's allocation count equal to M6's, which allocates
    // T at k and reuses it as R at k + 1; fused, R is allocated instead. C's
    // ownership can be judged at k: by 4, M6's op k reuses neither A nor B, so
    // it changes no owned slot other than T's, and by 2, C is not T.
    // Returns the operand slot (0 or 1) of T in operation k + 1.
    const auto fusible = [&](std::size_t k) -> std::optional<std::size_t> {
        if (k + 1 >= s.operations.size()) return std::nullopt;
        const Operation& first = s.operations[k];
        const Operation& second = s.operations[k + 1];
        if (second.opcode != Opcode::add) return std::nullopt;
        const ValueId t = first.result;
        if ((second.operands[0] == t) == (second.operands[1] == t)) return std::nullopt;
        const std::size_t slot = second.operands[0] == t ? 0 : 1;
        if (last_use[t.index()] != OperationId{static_cast<std::uint32_t>(k + 1)}) return std::nullopt;
        if (reusable(first.operands[0], k) || reusable(first.operands[1], k)) return std::nullopt;
        if (reusable(second.operands[1 - slot], k + 1)) return std::nullopt;
        if constexpr (!f32_intermediate_is_exact) {
            if (s.types[s.values[t.index()].type.index()].scalar == ScalarType::f32) return std::nullopt;
        }
        return slot;
    };

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
                if (const std::optional<std::size_t> slot = fusible(i)) {
                    // M7: execute ops i and i + 1 as one loop. Nothing is
                    // allocated or bound before every operand is checked; T
                    // stays unbound and unowned, as after M6's reuse of T.
                    const Operation& second = s.operations[i + 1];
                    const Buffer* other = lookup(second.operands[1 - *slot]);
                    if (other == nullptr) return internal_error("op " + std::to_string(i + 1) + " operand unbound");
                    std::optional<Buffer> sum = add_add_buffers(*lhs, *rhs, *other, *slot == 1);
                    if (!sum) {
                        return internal_error("ops " + std::to_string(i) + "-" + std::to_string(i + 1) +
                                              " operand buffers disagree");
                    }
                    owned[second.result.index()] = std::move(sum);
                    bound[second.result.index()] = &*owned[second.result.index()];
                    if (fused != nullptr) fused->push_back(static_cast<std::uint32_t>(i));
                    ++i;  // operation i + 1 has been executed
                    break;
                }
                // M6: reuse an operand's storage when allowed. Preference: lhs, then rhs.
                const ValueId reuse = reusable(op.operands[0], i) ? op.operands[0]
                                      : reusable(op.operands[1], i) ? op.operands[1]
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

}  // namespace lume

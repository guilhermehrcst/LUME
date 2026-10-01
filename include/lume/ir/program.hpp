#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "lume/ir/ids.hpp"
#include "lume/ir/types.hpp"

namespace lume {

// Zero is deliberately not a valid opcode.
enum class Opcode : std::uint8_t {
    input = 1,   // no operands; produces one value bound to the next caller buffer
    add = 2,     // two operands of identical type; produces one value of that type
    output = 3,  // one operand; produces no value
};

[[nodiscard]] constexpr std::string_view to_string(Opcode opcode) noexcept {
    switch (opcode) {
        case Opcode::input: return "input";
        case Opcode::add: return "add";
        case Opcode::output: return "output";
    }
    return "<invalid-opcode>";
}

// Metadata for one SSA value. Every value is produced by exactly one operation.
struct Value {
    TypeId type;
    OperationId definer;
};

// One operation. M0 operations have at most two operands, stored inline;
// unused operand slots and the result of a value-less operation are invalid ids.
struct Operation {
    Opcode opcode{};
    std::array<ValueId, 2> operands{};
    ValueId result;
};

// The complete IR state: three contiguous tables addressed by typed ids.
// Order is creation order, so ids and iteration are deterministic.
struct ProgramStorage {
    std::vector<Type> types;
    std::vector<Value> values;
    std::vector<Operation> operations;
};

// A straight-line Lume program and its builder API.
//
// The builder records exactly what it is asked to record and never rejects
// input; the verifier is the sole authority on validity. Only a
// VerifiedProgram (see lume/verify/verifier.hpp) can be executed.
class Program {
public:
    Program() = default;

    // Adopts raw tables without checking them. Exists so that states the
    // builder cannot produce (for example a corrupted definer link) can be
    // presented to the verifier.
    [[nodiscard]] static Program from_storage(ProgramStorage storage);

    // Appends an input buffer of type scalar[length].
    ValueId input(ScalarType scalar, std::uint32_t length);

    // Appends lhs + rhs. The result takes lhs's type; the verifier checks
    // that lhs and rhs agree.
    ValueId add(ValueId lhs, ValueId rhs);

    // Marks `value` as a program result. Outputs are returned in this order.
    OperationId output(ValueId value);

    [[nodiscard]] const ProgramStorage& storage() const noexcept { return storage_; }

private:
    TypeId intern(Type type);
    ValueId append(Opcode opcode, std::array<ValueId, 2> operands, bool produces_value, TypeId result_type);

    ProgramStorage storage_;
};

// Bytes held by the IR tables: `used` counts live elements, `reserved`
// counts allocated capacity. Excludes the Program object itself and allocator
// bookkeeping.
struct StorageFootprint {
    std::size_t used_bytes = 0;
    std::size_t reserved_bytes = 0;
};

[[nodiscard]] StorageFootprint storage_footprint(const Program& program) noexcept;

}  // namespace lume

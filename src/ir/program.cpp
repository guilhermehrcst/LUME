#include "pxir/ir/program.hpp"

#include <stdexcept>
#include <utility>

namespace pxir {

namespace {

// Tables may hold at most `invalid_value` entries so that every index is
// representable and distinct from the sentinel.
constexpr std::size_t max_table_size = ValueId::invalid_value;

template <class T>
std::size_t used_bytes(const std::vector<T>& table) noexcept {
    return table.size() * sizeof(T);
}

template <class T>
std::size_t reserved_bytes(const std::vector<T>& table) noexcept {
    return table.capacity() * sizeof(T);
}

}  // namespace

Program Program::from_storage(ProgramStorage storage) {
    Program program;
    program.storage_ = std::move(storage);
    return program;
}

ValueId Program::input(ScalarType scalar, std::uint32_t length) {
    return append(Opcode::input, {}, true, intern(Type{scalar, length}));
}

ValueId Program::add(ValueId lhs, ValueId rhs) {
    TypeId result_type = TypeId::invalid();
    if (lhs.index() < storage_.values.size()) result_type = storage_.values[lhs.index()].type;
    return append(Opcode::add, {lhs, rhs}, true, result_type);
}

OperationId Program::output(ValueId value) {
    append(Opcode::output, {value, ValueId::invalid()}, false, TypeId::invalid());
    return OperationId{static_cast<std::uint32_t>(storage_.operations.size() - 1)};
}

// Linear search: M0 programs have a handful of distinct types.
TypeId Program::intern(Type type) {
    for (std::size_t i = 0; i < storage_.types.size(); ++i) {
        if (storage_.types[i] == type) return TypeId{static_cast<std::uint32_t>(i)};
    }
    if (storage_.types.size() >= max_table_size) throw std::length_error("pxir: type table full");
    storage_.types.push_back(type);
    return TypeId{static_cast<std::uint32_t>(storage_.types.size() - 1)};
}

ValueId Program::append(Opcode opcode, std::array<ValueId, 2> operands, bool produces_value, TypeId result_type) {
    if (storage_.operations.size() >= max_table_size || storage_.values.size() >= max_table_size) {
        throw std::length_error("pxir: program table full");
    }

    const OperationId op{static_cast<std::uint32_t>(storage_.operations.size())};
    ValueId result = ValueId::invalid();
    if (produces_value) {
        result = ValueId{static_cast<std::uint32_t>(storage_.values.size())};
        storage_.values.push_back(Value{result_type, op});
    }

    // Strong guarantee: never leave a value without its defining operation.
    try {
        storage_.operations.push_back(Operation{opcode, operands, result});
    } catch (...) {
        if (produces_value) storage_.values.pop_back();
        throw;
    }
    return result;
}

StorageFootprint storage_footprint(const Program& program) noexcept {
    const ProgramStorage& s = program.storage();
    return StorageFootprint{
        used_bytes(s.types) + used_bytes(s.values) + used_bytes(s.operations),
        reserved_bytes(s.types) + reserved_bytes(s.values) + reserved_bytes(s.operations),
    };
}

}  // namespace pxir

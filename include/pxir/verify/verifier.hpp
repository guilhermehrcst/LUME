#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pxir/ir/ids.hpp"
#include "pxir/ir/program.hpp"

namespace pxir {

enum class DiagnosticCode : std::uint8_t {
    table_too_large,        // a table has more entries than 32-bit ids can address
    invalid_scalar_type,    // type table entry has an unknown ScalarType
    zero_length,            // type table entry has length 0
    byte_size_overflow,     // length * element size does not fit in size_t
    invalid_type_id,        // value references a nonexistent type
    invalid_definer,        // value references a nonexistent operation
    definer_mismatch,       // value's definer does not name the value as its result
    invalid_opcode,         // operation has an unknown opcode
    missing_operand,        // required operand slot holds the invalid sentinel
    invalid_operand,        // operand references a nonexistent value
    unexpected_operand,     // operand slot must be empty for this opcode
    use_before_definition,  // operand is defined by this or a later operation
    missing_result,         // opcode produces a value but result is empty
    invalid_result,         // result references a nonexistent value
    unexpected_result,      // opcode produces no value but result is set
    result_mismatch,        // result value's definer is a different operation
    scalar_type_mismatch,   // add operands have different scalar types
    shape_mismatch,         // add operands have different lengths
    result_type_mismatch,   // add result type differs from its operand type
    no_outputs,             // program has no output operation
};

[[nodiscard]] std::string_view to_string(DiagnosticCode code) noexcept;

struct Diagnostic {
    DiagnosticCode code;
    // Location of the problem; ids that do not apply are invalid.
    OperationId operation;
    ValueId value;
    TypeId type;
    std::string message;
};

struct VerifyResult;
[[nodiscard]] VerifyResult verify(Program program);

// A Program that passed verification. It can only be created by verify() and
// exposes the program read-only, so an executor that accepts a
// VerifiedProgram cannot receive unverified IR.
class VerifiedProgram {
public:
    [[nodiscard]] const Program& program() const noexcept { return program_; }

private:
    explicit VerifiedProgram(Program program) : program_(std::move(program)) {}
    friend VerifyResult verify(Program program);

    Program program_;
};

struct VerifyResult {
    std::optional<VerifiedProgram> program;  // engaged iff diagnostics is empty
    std::vector<Diagnostic> diagnostics;     // deterministic order: types, values, operations, program

    [[nodiscard]] bool ok() const noexcept { return program.has_value(); }
};

// Checks every M0 IR invariant. On success, takes ownership of the program.
// Diagnostics for a given input are identical on every run.
VerifyResult verify(Program program);

}  // namespace pxir

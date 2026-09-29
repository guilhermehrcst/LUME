#include "pxir/verify/verifier.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace pxir {

std::string_view to_string(DiagnosticCode code) noexcept {
    switch (code) {
        case DiagnosticCode::table_too_large: return "table_too_large";
        case DiagnosticCode::invalid_scalar_type: return "invalid_scalar_type";
        case DiagnosticCode::zero_length: return "zero_length";
        case DiagnosticCode::byte_size_overflow: return "byte_size_overflow";
        case DiagnosticCode::invalid_type_id: return "invalid_type_id";
        case DiagnosticCode::invalid_definer: return "invalid_definer";
        case DiagnosticCode::definer_mismatch: return "definer_mismatch";
        case DiagnosticCode::invalid_opcode: return "invalid_opcode";
        case DiagnosticCode::missing_operand: return "missing_operand";
        case DiagnosticCode::invalid_operand: return "invalid_operand";
        case DiagnosticCode::unexpected_operand: return "unexpected_operand";
        case DiagnosticCode::use_before_definition: return "use_before_definition";
        case DiagnosticCode::missing_result: return "missing_result";
        case DiagnosticCode::invalid_result: return "invalid_result";
        case DiagnosticCode::unexpected_result: return "unexpected_result";
        case DiagnosticCode::result_mismatch: return "result_mismatch";
        case DiagnosticCode::scalar_type_mismatch: return "scalar_type_mismatch";
        case DiagnosticCode::shape_mismatch: return "shape_mismatch";
        case DiagnosticCode::result_type_mismatch: return "result_type_mismatch";
        case DiagnosticCode::no_outputs: return "no_outputs";
    }
    return "<invalid-diagnostic-code>";
}

namespace {

std::string id_text(char sigil, std::uint32_t raw, bool valid) {
    if (!valid) return "<none>";
    return sigil + std::to_string(raw);
}

std::string value_text(ValueId v) { return id_text('%', v.value, v.is_valid()); }
std::string type_text(TypeId t) { return id_text('#', t.value, t.is_valid()); }

std::string type_text(Type t) {
    return std::string(to_string(t.scalar)) + "[" + std::to_string(t.length) + "]";
}

class Checker {
public:
    explicit Checker(const ProgramStorage& s) : s_(s) {}

    std::vector<Diagnostic> run() {
        if (!check_table_sizes()) return std::move(diagnostics_);
        check_types();
        check_values();
        check_operations();
        check_program();
        return std::move(diagnostics_);
    }

private:
    void report(DiagnosticCode code, OperationId op, ValueId value, TypeId type, std::string detail) {
        std::string message = "error[";
        message += to_string(code);
        message += "]";
        if (op.is_valid()) {
            message += " op ";
            message += std::to_string(op.value);
            if (op.index() < s_.operations.size()) {
                message += " (";
                message += to_string(s_.operations[op.index()].opcode);
                message += ")";
            }
        } else if (value.is_valid()) {
            message += " value " + value_text(value);
        } else if (type.is_valid()) {
            message += " type " + type_text(type);
        }
        message += ": ";
        message += detail;
        diagnostics_.push_back(Diagnostic{code, op, value, type, std::move(message)});
    }

    bool value_in_range(ValueId v) const noexcept { return v.index() < s_.values.size(); }
    bool type_in_range(TypeId t) const noexcept { return t.index() < s_.types.size(); }
    bool op_in_range(OperationId o) const noexcept { return o.index() < s_.operations.size(); }

    // Indices must stay below the sentinel; otherwise no id in this program can be trusted.
    bool check_table_sizes() {
        constexpr std::size_t limit = ValueId::invalid_value;
        const std::pair<const char*, std::size_t> tables[] = {
            {"type", s_.types.size()}, {"value", s_.values.size()}, {"operation", s_.operations.size()}};
        bool ok = true;
        for (const auto& [name, size] : tables) {
            if (size > limit) {
                report(DiagnosticCode::table_too_large, {}, {}, {},
                       std::string(name) + " table has " + std::to_string(size) + " entries; limit is " +
                           std::to_string(limit));
                ok = false;
            }
        }
        return ok;
    }

    void check_types() {
        for (std::size_t i = 0; i < s_.types.size(); ++i) {
            const TypeId id{static_cast<std::uint32_t>(i)};
            const Type t = s_.types[i];
            if (!is_known(t.scalar)) {
                report(DiagnosticCode::invalid_scalar_type, {}, {}, id,
                       "unknown scalar type code " + std::to_string(static_cast<unsigned>(t.scalar)));
            } else if (t.length == 0) {
                report(DiagnosticCode::zero_length, {}, {}, id, "buffer length must be greater than zero");
            } else if (!byte_size(t)) {
                report(DiagnosticCode::byte_size_overflow, {}, {}, id,
                       type_text(t) + " byte size does not fit in size_t");
            }
        }
    }

    void check_values() {
        for (std::size_t i = 0; i < s_.values.size(); ++i) {
            const ValueId id{static_cast<std::uint32_t>(i)};
            const Value& v = s_.values[i];
            if (!type_in_range(v.type)) {
                report(DiagnosticCode::invalid_type_id, {}, id, {},
                       "type " + type_text(v.type) + " does not exist (type table size " +
                           std::to_string(s_.types.size()) + ")");
            }
            if (!op_in_range(v.definer)) {
                report(DiagnosticCode::invalid_definer, {}, id, {},
                       "defining operation " + id_text('@', v.definer.value, v.definer.is_valid()) +
                           " does not exist (operation table size " + std::to_string(s_.operations.size()) + ")");
            } else if (s_.operations[v.definer.index()].result != id) {
                report(DiagnosticCode::definer_mismatch, {}, id, {},
                       "defining operation " + std::to_string(v.definer.value) + " names " +
                           value_text(s_.operations[v.definer.index()].result) + " as its result");
            }
        }
    }

    // Returns true when the operand refers to an existing value.
    bool check_operand(OperationId op, std::size_t slot, bool required) {
        const ValueId v = s_.operations[op.index()].operands[slot];
        const std::string where = "operand " + std::to_string(slot);
        if (!required) {
            if (v.is_valid()) {
                report(DiagnosticCode::unexpected_operand, op, v, {}, where + " must be empty, found " + value_text(v));
            }
            return false;
        }
        if (!v.is_valid()) {
            report(DiagnosticCode::missing_operand, op, {}, {}, where + " is missing");
            return false;
        }
        if (!value_in_range(v)) {
            report(DiagnosticCode::invalid_operand, op, v, {},
                   where + " " + value_text(v) + " does not exist (value table size " +
                       std::to_string(s_.values.size()) + ")");
            return false;
        }
        // Straight-line program: an operand must be defined by an earlier
        // operation. This also rules out self-reference and cycles.
        const OperationId definer = s_.values[v.index()].definer;
        if (op_in_range(definer) && definer.index() >= op.index()) {
            report(DiagnosticCode::use_before_definition, op, v, {},
                   where + " " + value_text(v) + " is defined by op " + std::to_string(definer.value) +
                       ", which does not precede its use");
            return false;
        }
        return op_in_range(definer);
    }

    // Returns true when the result refers to an existing value defined by `op`.
    bool check_result(OperationId op, bool required) {
        const ValueId r = s_.operations[op.index()].result;
        if (!required) {
            if (r.is_valid()) report(DiagnosticCode::unexpected_result, op, r, {}, "must not produce a value, found " + value_text(r));
            return false;
        }
        if (!r.is_valid()) {
            report(DiagnosticCode::missing_result, op, {}, {}, "result is missing");
            return false;
        }
        if (!value_in_range(r)) {
            report(DiagnosticCode::invalid_result, op, r, {},
                   "result " + value_text(r) + " does not exist (value table size " + std::to_string(s_.values.size()) +
                       ")");
            return false;
        }
        const OperationId definer = s_.values[r.index()].definer;
        if (definer != op) {
            report(DiagnosticCode::result_mismatch, op, r, {},
                   "result " + value_text(r) + " is defined by op " + id_text('@', definer.value, definer.is_valid()));
            return false;
        }
        return true;
    }

    // Type of a value already known to exist; nullptr if its type id is bad (reported elsewhere).
    const Type* type_of(ValueId v) const noexcept {
        const TypeId t = s_.values[v.index()].type;
        return type_in_range(t) ? &s_.types[t.index()] : nullptr;
    }

    void check_add_types(OperationId op, ValueId lhs, ValueId rhs, ValueId result, bool result_ok) {
        const Type* a = type_of(lhs);
        const Type* b = type_of(rhs);
        if (a == nullptr || b == nullptr) return;
        bool operands_agree = true;
        if (a->scalar != b->scalar) {
            report(DiagnosticCode::scalar_type_mismatch, op, {}, {},
                   "cannot add " + value_text(lhs) + ": " + type_text(*a) + " and " + value_text(rhs) + ": " +
                       type_text(*b) + " (scalar types differ)");
            operands_agree = false;
        }
        if (a->length != b->length) {
            report(DiagnosticCode::shape_mismatch, op, {}, {},
                   "cannot add " + value_text(lhs) + ": " + type_text(*a) + " and " + value_text(rhs) + ": " +
                       type_text(*b) + " (lengths differ)");
            operands_agree = false;
        }
        if (!operands_agree || !result_ok) return;
        const Type* r = type_of(result);
        if (r != nullptr && *r != *a) {
            report(DiagnosticCode::result_type_mismatch, op, result, {},
                   "result " + value_text(result) + ": " + type_text(*r) + " does not match operand type " +
                       type_text(*a));
        }
    }

    void check_operations() {
        for (std::size_t i = 0; i < s_.operations.size(); ++i) {
            const OperationId id{static_cast<std::uint32_t>(i)};
            const Operation& op = s_.operations[i];
            switch (op.opcode) {
                case Opcode::input:
                    check_operand(id, 0, false);
                    check_operand(id, 1, false);
                    check_result(id, true);
                    break;
                case Opcode::add: {
                    const bool lhs_ok = check_operand(id, 0, true);
                    const bool rhs_ok = check_operand(id, 1, true);
                    const bool result_ok = check_result(id, true);
                    if (lhs_ok && rhs_ok) check_add_types(id, op.operands[0], op.operands[1], op.result, result_ok);
                    break;
                }
                case Opcode::output:
                    check_operand(id, 0, true);
                    check_operand(id, 1, false);
                    check_result(id, false);
                    break;
                default:
                    report(DiagnosticCode::invalid_opcode, id, {}, {},
                           "unknown opcode " + std::to_string(static_cast<unsigned>(op.opcode)));
                    break;
            }
        }
    }

    void check_program() {
        for (const Operation& op : s_.operations) {
            if (op.opcode == Opcode::output) return;
        }
        report(DiagnosticCode::no_outputs, {}, {}, {}, "program has no output operation");
    }

    const ProgramStorage& s_;
    std::vector<Diagnostic> diagnostics_;
};

}  // namespace

VerifyResult verify(Program program) {
    VerifyResult result;
    result.diagnostics = Checker(program.storage()).run();
    if (result.diagnostics.empty()) result.program = VerifiedProgram(std::move(program));
    return result;
}

}  // namespace pxir

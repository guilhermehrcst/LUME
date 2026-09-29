#include "pxir/ir/dump.hpp"

#include <string>

namespace pxir {

namespace {

void append_value(std::string& out, ValueId value) {
    if (!value.is_valid()) {
        out += "<none>";
        return;
    }
    out += '%';
    out += std::to_string(value.value);
}

void append_type(std::string& out, const ProgramStorage& s, ValueId value) {
    if (value.index() >= s.values.size() || s.values[value.index()].type.index() >= s.types.size()) {
        out += "<invalid-type>";
        return;
    }
    const Type type = s.types[s.values[value.index()].type.index()];
    out += to_string(type.scalar);
    out += '[';
    out += std::to_string(type.length);
    out += ']';
}

}  // namespace

std::string to_debug_string(const Program& program) {
    const ProgramStorage& s = program.storage();
    std::string out;
    for (const Operation& op : s.operations) {
        switch (op.opcode) {
            case Opcode::input:
                append_value(out, op.result);
                out += " = input ";
                append_type(out, s, op.result);
                break;
            case Opcode::add:
                append_value(out, op.result);
                out += " = add ";
                append_value(out, op.operands[0]);
                out += ", ";
                append_value(out, op.operands[1]);
                break;
            case Opcode::output:
                out += "output ";
                append_value(out, op.operands[0]);
                break;
            default:
                out += "<invalid-opcode ";
                out += std::to_string(static_cast<unsigned>(op.opcode));
                out += '>';
                break;
        }
        out += '\n';
    }
    return out;
}

}  // namespace pxir

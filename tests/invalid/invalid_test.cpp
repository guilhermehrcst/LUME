// Invalid representations must fail predictably. Every case here expects
// rejection; a passing test means the verifier refused the program.

#include <span>
#include <type_traits>
#include <utility>

#include "check.hpp"
#include "diagnostics.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"

using lume::DiagnosticCode;
using lume::Opcode;
using lume::OperationId;
using lume::TypeId;
using lume::ValueId;
using lume_test::rejected_with;

// ---- Compile-time barriers --------------------------------------------------

// Ids are not interchangeable with each other or with raw integers.
static_assert(!std::is_convertible_v<ValueId, OperationId>);
static_assert(!std::is_convertible_v<OperationId, ValueId>);
static_assert(!std::is_convertible_v<TypeId, ValueId>);
static_assert(!std::is_convertible_v<std::uint32_t, ValueId>);
static_assert(!std::is_constructible_v<ValueId, OperationId>);

// Execution before verification is unrepresentable: a VerifiedProgram can only
// come from verify(), and the executor accepts nothing else.
static_assert(!std::is_default_constructible_v<lume::VerifiedProgram>);
static_assert(!std::is_constructible_v<lume::VerifiedProgram, lume::Program>);
static_assert(!std::is_constructible_v<lume::VerifiedProgram, const lume::Program&>);
static_assert(!std::is_invocable_v<decltype(&lume::execute_cpu_reference), const lume::Program&,
                                   std::span<const lume::Buffer>>);

namespace {

// ---- Builder-reachable invalid programs -------------------------------------

void rejects_mismatched_shapes() {
    lume::Program p;
    const auto a = p.input(lume::f32, 100);
    const auto b = p.input(lume::f32, 200);
    p.output(p.add(a, b));
    LUME_CHECK(rejected_with(lume::verify(p), DiagnosticCode::shape_mismatch));
}

void rejects_mismatched_scalar_types() {
    lume::Program p;
    const auto a = p.input(lume::f32, 100);
    const auto b = p.input(lume::i32, 100);
    p.output(p.add(a, b));
    const lume::VerifyResult r = lume::verify(p);
    LUME_CHECK(rejected_with(r, DiagnosticCode::scalar_type_mismatch));
    LUME_CHECK(r.diagnostics.size() == 1);
}

void reports_both_scalar_and_shape_mismatch() {
    lume::Program p;
    const auto a = p.input(lume::f32, 100);
    const auto b = p.input(lume::i32, 200);
    p.output(p.add(a, b));
    const lume::VerifyResult r = lume::verify(p);
    LUME_CHECK(rejected_with(r, DiagnosticCode::scalar_type_mismatch));
    LUME_CHECK(rejected_with(r, DiagnosticCode::shape_mismatch));
}

void rejects_nonexistent_operand() {
    lume::Program p;
    const auto a = p.input(lume::f32, 8);
    p.output(p.add(a, ValueId{99}));
    LUME_CHECK(rejected_with(lume::verify(p), DiagnosticCode::invalid_operand));
}

void rejects_missing_operand() {
    lume::Program p;
    const auto a = p.input(lume::f32, 8);
    p.output(p.add(ValueId{}, a));
    const lume::VerifyResult r = lume::verify(p);
    LUME_CHECK(rejected_with(r, DiagnosticCode::missing_operand));
    // The builder could not derive a result type from the missing lhs.
    LUME_CHECK(rejected_with(r, DiagnosticCode::invalid_type_id));
}

void rejects_output_of_nonexistent_value() {
    lume::Program p;
    p.input(lume::f32, 8);
    p.output(ValueId{42});
    LUME_CHECK(rejected_with(lume::verify(p), DiagnosticCode::invalid_operand));
}

void rejects_output_of_missing_value() {
    lume::Program p;
    p.input(lume::f32, 8);
    p.output(ValueId{});
    LUME_CHECK(rejected_with(lume::verify(p), DiagnosticCode::missing_operand));
}

void rejects_program_without_outputs() {
    lume::Program p;
    p.add(p.input(lume::f32, 8), p.input(lume::f32, 8));
    LUME_CHECK(rejected_with(lume::verify(p), DiagnosticCode::no_outputs));
    LUME_CHECK(rejected_with(lume::verify(lume::Program{}), DiagnosticCode::no_outputs));
}

void rejects_zero_length() {
    lume::Program p;
    p.output(p.input(lume::f32, 0));
    LUME_CHECK(rejected_with(lume::verify(p), DiagnosticCode::zero_length));
}

void rejects_unknown_scalar_type() {
    lume::Program p;
    p.output(p.input(static_cast<lume::ScalarType>(0), 4));
    p.output(p.input(static_cast<lume::ScalarType>(7), 4));
    const lume::VerifyResult r = lume::verify(p);
    LUME_CHECK(rejected_with(r, DiagnosticCode::invalid_scalar_type));
    LUME_CHECK(r.diagnostics.size() == 2);
}

void byte_size_rejects_unknown_scalar() {
    LUME_CHECK(!lume::byte_size(lume::Type{static_cast<lume::ScalarType>(9), 4}).has_value());
    if constexpr (sizeof(std::size_t) >= 8) {
        LUME_CHECK(lume::byte_size(lume::Type{lume::f32, 0xffffffffu}) == std::size_t{0xffffffffu} * 4u);
    } else {
        LUME_CHECK(!lume::byte_size(lume::Type{lume::f32, 0xffffffffu}).has_value());
    }
}

// ---- Structurally malformed storage (unreachable through the builder) -------

// A well-formed vector add, as raw storage, for targeted corruption.
lume::ProgramStorage valid_storage() {
    lume::ProgramStorage s;
    s.types = {lume::Type{lume::f32, 4}};
    s.values = {lume::Value{TypeId{0}, OperationId{0}}, lume::Value{TypeId{0}, OperationId{1}},
                lume::Value{TypeId{0}, OperationId{2}}};
    s.operations = {
        lume::Operation{Opcode::input, {}, ValueId{0}},
        lume::Operation{Opcode::input, {}, ValueId{1}},
        lume::Operation{Opcode::add, {ValueId{0}, ValueId{1}}, ValueId{2}},
        lume::Operation{Opcode::output, {ValueId{2}, ValueId{}}, ValueId{}},
    };
    return s;
}

lume::VerifyResult verify_storage(lume::ProgramStorage s) { return lume::verify(lume::Program::from_storage(std::move(s))); }

void baseline_storage_is_valid() { LUME_CHECK(verify_storage(valid_storage()).ok()); }

void rejects_value_with_nonexistent_type() {
    auto s = valid_storage();
    s.values[1].type = TypeId{5};
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::invalid_type_id));
}

void rejects_value_with_nonexistent_definer() {
    auto s = valid_storage();
    s.values[0].definer = OperationId{17};
    const lume::VerifyResult r = verify_storage(s);
    LUME_CHECK(rejected_with(r, DiagnosticCode::invalid_definer));
    LUME_CHECK(rejected_with(r, DiagnosticCode::result_mismatch));  // op 0 still claims %0

    s = valid_storage();
    s.values[0].definer = OperationId{};
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::invalid_definer));
}

void rejects_definer_mismatch() {
    auto s = valid_storage();
    s.values[1].definer = OperationId{0};  // op 0 names %0 as its result, not %1
    const lume::VerifyResult r = verify_storage(s);
    LUME_CHECK(rejected_with(r, DiagnosticCode::definer_mismatch));
    LUME_CHECK(rejected_with(r, DiagnosticCode::result_mismatch));  // op 1 claims %1
}

void rejects_use_of_own_result() {
    auto s = valid_storage();
    s.operations[2].operands[1] = ValueId{2};  // %2 = add %0, %2
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::use_before_definition));
}

void rejects_forward_reference() {
    auto s = valid_storage();
    // Move the add after the output: output now uses %2 before op 3 defines it.
    std::swap(s.operations[2], s.operations[3]);
    s.values[2].definer = OperationId{3};
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::use_before_definition));
}

void rejects_unknown_opcode() {
    auto s = valid_storage();
    s.operations.push_back(lume::Operation{static_cast<Opcode>(0), {}, ValueId{}});
    s.operations.push_back(lume::Operation{static_cast<Opcode>(200), {}, ValueId{}});
    const lume::VerifyResult r = verify_storage(s);
    LUME_CHECK(rejected_with(r, DiagnosticCode::invalid_opcode));
    LUME_CHECK(r.diagnostics.size() == 2);
}

void rejects_operand_on_input() {
    auto s = valid_storage();
    s.operations[1].operands[0] = ValueId{0};
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::unexpected_operand));
}

void rejects_second_operand_on_output() {
    auto s = valid_storage();
    s.operations[3].operands[1] = ValueId{0};
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::unexpected_operand));
}

void rejects_result_on_output() {
    auto s = valid_storage();
    s.operations[3].result = ValueId{2};
    const lume::VerifyResult r = verify_storage(s);
    LUME_CHECK(rejected_with(r, DiagnosticCode::unexpected_result));
}

void rejects_missing_and_nonexistent_result() {
    auto s = valid_storage();
    s.operations[2].result = ValueId{};
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::missing_result));

    s = valid_storage();
    s.operations[0].result = ValueId{50};
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::invalid_result));
}

void rejects_result_defined_elsewhere() {
    auto s = valid_storage();
    s.operations[2].result = ValueId{0};  // add claims input 0's value
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::result_mismatch));
}

void rejects_add_result_type_mismatch() {
    auto s = valid_storage();
    s.types.push_back(lume::Type{lume::f32, 8});
    s.values[2].type = TypeId{1};  // %2: f32[8] = add f32[4], f32[4]
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::result_type_mismatch));
}

void rejects_malformed_type_table_entry() {
    auto s = valid_storage();
    s.types[0].length = 0;
    LUME_CHECK(rejected_with(verify_storage(s), DiagnosticCode::zero_length));
}

}  // namespace

int main() {
    rejects_mismatched_shapes();
    rejects_mismatched_scalar_types();
    reports_both_scalar_and_shape_mismatch();
    rejects_nonexistent_operand();
    rejects_missing_operand();
    rejects_output_of_nonexistent_value();
    rejects_output_of_missing_value();
    rejects_program_without_outputs();
    rejects_zero_length();
    rejects_unknown_scalar_type();
    byte_size_rejects_unknown_scalar();

    baseline_storage_is_valid();
    rejects_value_with_nonexistent_type();
    rejects_value_with_nonexistent_definer();
    rejects_definer_mismatch();
    rejects_use_of_own_result();
    rejects_forward_reference();
    rejects_unknown_opcode();
    rejects_operand_on_input();
    rejects_second_operand_on_output();
    rejects_result_on_output();
    rejects_missing_and_nonexistent_result();
    rejects_result_defined_elsewhere();
    rejects_add_result_type_mismatch();
    rejects_malformed_type_table_entry();
    return lume_test::finish("invalid");
}

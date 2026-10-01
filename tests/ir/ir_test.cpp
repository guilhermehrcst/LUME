// IR construction: ids, tables, type interning, debug dump, footprint.

#include <cstdio>
#include <string>

#include "check.hpp"
#include "lume/ir/dump.hpp"
#include "lume/ir/program.hpp"

namespace {

void builds_vector_add_with_deterministic_ids() {
    lume::Program program;
    const lume::ValueId a = program.input(lume::f32, 1024);
    const lume::ValueId b = program.input(lume::f32, 1024);
    const lume::ValueId c = program.add(a, b);
    const lume::OperationId out = program.output(c);

    LUME_CHECK(a == lume::ValueId{0});
    LUME_CHECK(b == lume::ValueId{1});
    LUME_CHECK(c == lume::ValueId{2});
    LUME_CHECK(out == lume::OperationId{3});

    const lume::ProgramStorage& s = program.storage();
    LUME_CHECK(s.operations.size() == 4);
    LUME_CHECK(s.values.size() == 3);
    LUME_CHECK(s.types.size() == 1);  // f32[1024] interned once

    LUME_CHECK(s.operations[0].opcode == lume::Opcode::input);
    LUME_CHECK(s.operations[2].opcode == lume::Opcode::add);
    LUME_CHECK(s.operations[2].operands[0] == a);
    LUME_CHECK(s.operations[2].operands[1] == b);
    LUME_CHECK(s.operations[2].result == c);
    LUME_CHECK(s.operations[3].opcode == lume::Opcode::output);
    LUME_CHECK(s.operations[3].operands[0] == c);
    LUME_CHECK(!s.operations[3].operands[1].is_valid());
    LUME_CHECK(!s.operations[3].result.is_valid());

    LUME_CHECK(s.values[2].definer == lume::OperationId{2});
    LUME_CHECK(s.values[2].type == lume::TypeId{0});
    LUME_CHECK((s.types[0] == lume::Type{lume::f32, 1024}));
}

void repeated_construction_is_identical() {
    const auto build = [] {
        lume::Program p;
        p.output(p.add(p.input(lume::f32, 8), p.input(lume::f32, 8)));
        return lume::to_debug_string(p);
    };
    LUME_CHECK(build() == build());
}

void represents_i32_and_distinct_types() {
    lume::Program program;
    const lume::ValueId x = program.input(lume::i32, 16);
    const lume::ValueId y = program.input(lume::f32, 16);
    const lume::ValueId z = program.input(lume::i32, 32);
    const lume::ProgramStorage& s = program.storage();
    LUME_CHECK(s.types.size() == 3);
    LUME_CHECK((s.types[s.values[x.index()].type.index()] == lume::Type{lume::i32, 16}));
    LUME_CHECK((s.types[s.values[y.index()].type.index()] == lume::Type{lume::f32, 16}));
    LUME_CHECK((s.types[s.values[z.index()].type.index()] == lume::Type{lume::i32, 32}));
}

void debug_dump_matches_expected_text() {
    lume::Program program;
    const auto a = program.input(lume::f32, 1024);
    const auto b = program.input(lume::f32, 1024);
    program.output(program.add(a, b));
    const std::string expected =
        "%0 = input f32[1024]\n"
        "%1 = input f32[1024]\n"
        "%2 = add %0, %1\n"
        "output %2\n";
    const std::string actual = lume::to_debug_string(program);
    if (!LUME_CHECK(actual == expected)) std::fprintf(stderr, "actual:\n%s", actual.c_str());
}

void debug_dump_tolerates_malformed_program() {
    lume::ProgramStorage s;
    s.operations.push_back(lume::Operation{lume::Opcode::add, {lume::ValueId{7}, lume::ValueId{}}, lume::ValueId{}});
    s.operations.push_back(lume::Operation{static_cast<lume::Opcode>(0), {}, lume::ValueId{}});
    s.operations.push_back(lume::Operation{lume::Opcode::input, {}, lume::ValueId{9}});
    const std::string actual = lume::to_debug_string(lume::Program::from_storage(s));
    LUME_CHECK(actual == "<none> = add %7, <none>\n<invalid-opcode 0>\n%9 = input <invalid-type>\n");
}

void footprint_counts_table_bytes() {
    lume::Program program;
    program.output(program.add(program.input(lume::f32, 4), program.input(lume::f32, 4)));
    const lume::StorageFootprint f = lume::storage_footprint(program);
    const std::size_t expected = 1 * sizeof(lume::Type) + 3 * sizeof(lume::Value) + 4 * sizeof(lume::Operation);
    LUME_CHECK(f.used_bytes == expected);
    LUME_CHECK(f.reserved_bytes >= f.used_bytes);
}

void ids_default_to_invalid() {
    LUME_CHECK(!lume::ValueId{}.is_valid());
    LUME_CHECK(!lume::OperationId::invalid().is_valid());
    LUME_CHECK(lume::TypeId{0}.is_valid());
}

}  // namespace

int main() {
    builds_vector_add_with_deterministic_ids();
    repeated_construction_is_identical();
    represents_i32_and_distinct_types();
    debug_dump_matches_expected_text();
    debug_dump_tolerates_malformed_program();
    footprint_counts_table_bytes();
    ids_default_to_invalid();
    return lume_test::finish("ir");
}

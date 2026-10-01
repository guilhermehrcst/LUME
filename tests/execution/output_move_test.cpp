// M2: an output moves an executor-owned buffer only when it is the value's
// last use; every other output copies. These tests check that results,
// ordering and borrowed inputs are unchanged by that rule.

#include <algorithm>
#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "check.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"

using lume::Buffer;

namespace {

constexpr std::uint32_t n = 1000;

struct Inputs {
    std::vector<float> a;
    std::vector<float> b;
};

Inputs make_inputs() {
    std::mt19937_64 engine(2024);
    Inputs in;
    in.a = lume_oracle::generate_f32(engine, n);
    in.b = lume_oracle::generate_f32(engine, n);
    return in;
}

std::vector<float> add(const std::vector<float>& x, const std::vector<float>& y) {
    std::vector<float> out(x.size());
    lume_oracle::native_add(x, y, out);
    return out;
}

// Verifies, executes, and checks that the caller's buffers are unchanged.
lume::ExecutionResult run(lume::Program program, const Inputs& in) {
    lume::VerifyResult verified = lume::verify(std::move(program));
    if (!LUME_CHECK(verified.ok())) return {};
    const std::vector<Buffer> buffers{Buffer(in.a), Buffer(in.b)};
    lume::ExecutionResult r = lume::execute_cpu_reference(*verified.program, buffers);
    LUME_CHECK(r.ok());
    LUME_CHECK(lume_oracle::exactly_equal(*buffers[0].f32_view(), in.a));
    LUME_CHECK(lume_oracle::exactly_equal(*buffers[1].f32_view(), in.b));
    return r;
}

bool same_i32(std::span<const std::int32_t> x, const std::vector<std::int32_t>& y) {
    return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin());
}

bool output_equals(const lume::ExecutionResult& r, std::size_t k, const std::vector<float>& expected) {
    return k < r.outputs.size() && r.outputs[k].f32_view().has_value() &&
           lume_oracle::exactly_equal(*r.outputs[k].f32_view(), expected);
}

// A: output C, its single and final use -> moved.
void single_owned_output() {
    const Inputs in = make_inputs();
    lume::Program p;
    const auto a = p.input(lume::f32, n);
    const auto b = p.input(lume::f32, n);
    p.output(p.add(a, b));
    const auto r = run(std::move(p), in);
    LUME_CHECK(r.outputs.size() == 1);
    LUME_CHECK(output_equals(r, 0, add(in.a, in.b)));
}

// B: output C twice -> first copies, second moves; both identical.
void same_owned_value_output_twice() {
    const Inputs in = make_inputs();
    lume::Program p;
    const auto c = p.add(p.input(lume::f32, n), p.input(lume::f32, n));
    p.output(c);
    p.output(c);
    const auto r = run(std::move(p), in);
    const auto expected = add(in.a, in.b);
    LUME_CHECK(r.outputs.size() == 2);
    LUME_CHECK(output_equals(r, 0, expected));
    LUME_CHECK(output_equals(r, 1, expected));
}

// C: output C, then C is read by a later add -> the output must copy.
void output_before_later_computational_use() {
    const Inputs in = make_inputs();
    lume::Program p;
    const auto a = p.input(lume::f32, n);
    const auto b = p.input(lume::f32, n);
    const auto c = p.add(a, b);
    p.output(c);
    p.output(p.add(c, a));
    const auto r = run(std::move(p), in);
    const auto expected_c = add(in.a, in.b);
    LUME_CHECK(r.outputs.size() == 2);
    LUME_CHECK(output_equals(r, 0, expected_c));
    LUME_CHECK(output_equals(r, 1, add(expected_c, in.a)));
}

// D: outputs of inputs are copies; the caller's buffers stay untouched
// (checked in run()), including when an input is output last.
void input_passthrough() {
    const Inputs in = make_inputs();
    lume::Program p;
    const auto a = p.input(lume::f32, n);
    const auto b = p.input(lume::f32, n);
    p.output(a);
    p.output(p.add(a, b));
    p.output(b);
    const auto r = run(std::move(p), in);
    LUME_CHECK(r.outputs.size() == 3);
    LUME_CHECK(output_equals(r, 0, in.a));
    LUME_CHECK(output_equals(r, 1, add(in.a, in.b)));
    LUME_CHECK(output_equals(r, 2, in.b));
}

// E: C is consumed only by D = C + C; output D -> exact.
void chained_computation() {
    const Inputs in = make_inputs();
    lume::Program p;
    const auto c = p.add(p.input(lume::f32, n), p.input(lume::f32, n));
    p.output(p.add(c, c));
    const auto r = run(std::move(p), in);
    const auto expected_c = add(in.a, in.b);
    LUME_CHECK(r.outputs.size() == 1);
    LUME_CHECK(output_equals(r, 0, add(expected_c, expected_c)));
}

// F: the move path for i32-owned buffers, including a copy-then-move pair.
void i32_owned_outputs() {
    std::mt19937_64 engine(99);
    const auto x = lume_oracle::generate_i32(engine, n);
    const auto y = lume_oracle::generate_i32(engine, n);
    std::vector<std::int32_t> expected(n);
    lume_oracle::native_add(x, y, expected);

    lume::Program p;
    const auto c = p.add(p.input(lume::i32, n), p.input(lume::i32, n));
    p.output(c);
    p.output(c);
    lume::VerifyResult verified = lume::verify(std::move(p));
    if (!LUME_CHECK(verified.ok())) return;
    const std::vector<Buffer> buffers{Buffer(x), Buffer(y)};
    const auto r = lume::execute_cpu_reference(*verified.program, buffers);
    if (!LUME_CHECK(r.ok()) || !LUME_CHECK(r.outputs.size() == 2)) return;
    LUME_CHECK(r.outputs[0].i32_view() && same_i32(*r.outputs[0].i32_view(), expected));
    LUME_CHECK(r.outputs[1].i32_view() && same_i32(*r.outputs[1].i32_view(), expected));
    LUME_CHECK(same_i32(*buffers[0].i32_view(), x));
    LUME_CHECK(same_i32(*buffers[1].i32_view(), y));
}

// G: outputs appear exactly in IR order, whether each is a move or a copy.
void multiple_output_ordering() {
    const Inputs in = make_inputs();
    lume::Program p;
    const auto a = p.input(lume::f32, n);
    const auto b = p.input(lume::f32, n);
    const auto c = p.add(a, b);  // a + b
    const auto d = p.add(c, b);  // (a + b) + b
    const auto e = p.add(a, a);  // a + a
    p.output(e);                 // move (last use of e)
    p.output(c);                 // move (c's last reader, d, already ran)
    p.output(b);                 // copy (input)
    p.output(d);                 // copy (read again below)
    p.output(d);                 // move
    const auto r = run(std::move(p), in);
    const auto ec = add(in.a, in.b);
    const auto ed = add(ec, in.b);
    LUME_CHECK(r.outputs.size() == 5);
    LUME_CHECK(output_equals(r, 0, add(in.a, in.a)));
    LUME_CHECK(output_equals(r, 1, ec));
    LUME_CHECK(output_equals(r, 2, in.b));
    LUME_CHECK(output_equals(r, 3, ed));
    LUME_CHECK(output_equals(r, 4, ed));
}

// The same verified program executes repeatedly with identical results: the
// move affects only the ExecutionResult of that call, never the program.
void repeated_execution_is_stable() {
    const Inputs in = make_inputs();
    lume::Program p;
    const auto c = p.add(p.input(lume::f32, n), p.input(lume::f32, n));
    p.output(c);
    p.output(c);
    lume::VerifyResult verified = lume::verify(std::move(p));
    if (!LUME_CHECK(verified.ok())) return;
    const std::vector<Buffer> buffers{Buffer(in.a), Buffer(in.b)};
    const auto expected = add(in.a, in.b);
    for (int i = 0; i < 3; ++i) {
        const auto r = lume::execute_cpu_reference(*verified.program, buffers);
        LUME_CHECK(r.ok());
        LUME_CHECK(output_equals(r, 0, expected));
        LUME_CHECK(output_equals(r, 1, expected));
    }
}

}  // namespace

int main() {
    single_owned_output();
    same_owned_value_output_twice();
    output_before_later_computational_use();
    input_passthrough();
    chained_computation();
    i32_owned_outputs();
    multiple_output_ordering();
    repeated_execution_is_stable();
    return lume_test::finish("output_move");
}

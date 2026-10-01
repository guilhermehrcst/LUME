// M4 Phase B: Buffer holds either a caller std::vector or an executor
// OwnedArray. Views must look identical for both; copies must be deep and
// independent; moves must transfer storage without copying and never throw.

#include <bit>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "check.hpp"
#include "lume/runtime/buffer.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"

using lume::Buffer;
using lume::OwnedArray;

static_assert(std::is_nothrow_move_constructible_v<Buffer>);
static_assert(std::is_nothrow_move_assignable_v<Buffer>);
static_assert(std::is_copy_constructible_v<Buffer> && std::is_copy_assignable_v<Buffer>);

namespace {

template <class T>
OwnedArray<T> owned(const std::vector<T>& values) {
    auto a = OwnedArray<T>::for_overwrite(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) a[i] = values[i];  // every element written
    return a;
}

template <class T>
std::optional<std::span<const T>> view_of(const Buffer& b) {
    if constexpr (std::is_same_v<T, float>) {
        return b.f32_view();
    } else {
        return b.i32_view();
    }
}

template <class T>
bool holds(const Buffer& b, const std::vector<T>& expected) {
    const auto v = view_of<T>(b);
    if (!v || v->size() != expected.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (std::bit_cast<std::uint32_t>((*v)[i]) != std::bit_cast<std::uint32_t>(expected[i])) return false;
    }
    return true;
}

template <class T>
const void* storage(const Buffer& b) {
    return view_of<T>(b)->data();
}

// Copy construction and copy assignment: same type, length and values;
// different storage (independent ownership).
template <class T>
void copy_is_deep(const Buffer& original, const std::vector<T>& values, lume::ScalarType scalar) {
    const Buffer copy = original;
    LUME_CHECK(copy.scalar() == scalar);
    LUME_CHECK(copy.length() == values.size());
    LUME_CHECK(holds(copy, values));
    LUME_CHECK(storage<T>(copy) != storage<T>(original));

    Buffer assigned(std::vector<T>{T{}});
    assigned = original;
    LUME_CHECK(assigned.scalar() == scalar && holds(assigned, values));
    LUME_CHECK(storage<T>(assigned) != storage<T>(original));

    // The original is unchanged by copying.
    LUME_CHECK(holds(original, values));
}

void copies_for_all_four_representations() {
    const std::vector<float> f{1.5f, -0.0f, 3.25f, 7.0f, 9.5f};
    const std::vector<std::int32_t> n{1, -2, 3, 2147483647, -2147483647 - 1};
    copy_is_deep(Buffer(f), f, lume::ScalarType::f32);             // vector-backed f32
    copy_is_deep(Buffer(n), n, lume::ScalarType::i32);             // vector-backed i32
    copy_is_deep(Buffer(owned(f)), f, lume::ScalarType::f32);      // OwnedArray-backed f32
    copy_is_deep(Buffer(owned(n)), n, lume::ScalarType::i32);      // OwnedArray-backed i32
}

void views_distinguish_scalar_type() {
    const Buffer vf(std::vector<float>{1.0f});
    const Buffer oi(owned(std::vector<std::int32_t>{1}));
    LUME_CHECK(vf.f32_view().has_value() && !vf.i32_view().has_value());
    LUME_CHECK(oi.i32_view().has_value() && !oi.f32_view().has_value());
    LUME_CHECK(vf.scalar() == lume::ScalarType::f32 && oi.scalar() == lume::ScalarType::i32);
    LUME_CHECK(vf.length() == 1 && oi.length() == 1);
}

void moves_transfer_storage() {
    const std::vector<float> f{1.0f, 2.0f, 3.0f};
    Buffer a(owned(f));
    const void* s = storage<float>(a);

    Buffer b(std::move(a));  // move construction: same storage, no copy
    LUME_CHECK(storage<float>(b) == s && holds(b, f));

    Buffer c(std::vector<float>{0.0f});
    c = std::move(b);  // move assignment
    LUME_CHECK(storage<float>(c) == s && holds(c, f));

    // Reallocation of a vector of Buffers moves them; storage identity survives.
    std::vector<Buffer> many;
    many.push_back(std::move(c));
    for (int i = 0; i < 64; ++i) many.emplace_back(std::vector<float>{static_cast<float>(i)});
    LUME_CHECK(storage<float>(many[0]) == s && holds(many[0], f));
}

// The executor's final-use output moves its OwnedArray result into
// ExecutionResult: the output is the executor's result storage, not a copy,
// and the caller's input buffers stay untouched.
void executor_outputs_owned_results() {
    lume::Program p;
    const auto x = p.input(lume::f32, 5);
    const auto y = p.input(lume::f32, 5);
    const auto sum = p.add(x, y);
    p.output(sum);  // copy (read again below)
    p.output(sum);  // move
    p.output(x);    // copy of a caller input
    auto verified = lume::verify(std::move(p));
    if (!LUME_CHECK(verified.ok())) return;

    const std::vector<float> a{1, 2, 3, 4, 5};
    const std::vector<float> b{10, 20, 30, 40, 50};
    const std::vector<Buffer> inputs{Buffer(a), Buffer(b)};
    const void* input_storage = storage<float>(inputs[0]);
    const auto r = lume::execute_cpu_reference(*verified.program, inputs);
    if (!LUME_CHECK(r.ok()) || !LUME_CHECK(r.outputs.size() == 3)) return;
    const std::vector<float> expected{11, 22, 33, 44, 55};
    LUME_CHECK(holds(r.outputs[0], expected));
    LUME_CHECK(holds(r.outputs[1], expected));
    LUME_CHECK(storage<float>(r.outputs[0]) != storage<float>(r.outputs[1]));
    LUME_CHECK(holds(r.outputs[2], a));
    LUME_CHECK(storage<float>(r.outputs[2]) != input_storage);  // inputs are copied, never moved
    LUME_CHECK(storage<float>(inputs[0]) == input_storage && holds(inputs[0], a) && holds(inputs[1], b));
}

// A, B: the integrated executor at lengths around vector widths, f32 and i32,
// against the oracle (length and exact bits).
void executor_edge_lengths() {
    for (const std::uint32_t n : {1u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 33u, 4099u}) {
        std::mt19937_64 engine(9000 + n);
        const auto fa = lume_oracle::generate_f32(engine, n);
        const auto fb = lume_oracle::generate_f32(engine, n);
        const auto ia = lume_oracle::generate_i32(engine, n);
        const auto ib = lume_oracle::generate_i32(engine, n);
        std::vector<float> fe(n);
        std::vector<std::int32_t> ie(n);
        lume_oracle::native_add(fa, fb, fe);
        lume_oracle::native_add(ia, ib, ie);

        for (const lume::ScalarType t : {lume::ScalarType::f32, lume::ScalarType::i32}) {
            lume::Program p;
            const auto x = p.input(t, n);
            const auto y = p.input(t, n);
            p.output(p.add(x, y));
            auto verified = lume::verify(std::move(p));
            if (!LUME_CHECK(verified.ok())) return;
            const std::vector<Buffer> inputs = t == lume::ScalarType::f32
                                                   ? std::vector<Buffer>{Buffer(fa), Buffer(fb)}
                                                   : std::vector<Buffer>{Buffer(ia), Buffer(ib)};
            const auto r = lume::execute_cpu_reference(*verified.program, inputs);
            const bool ok = r.ok() && r.outputs.size() == 1 &&
                            (t == lume::ScalarType::f32 ? holds(r.outputs[0], fe) : holds(r.outputs[0], ie));
            if (!LUME_CHECK(ok)) std::fprintf(stderr, "  executor %s n=%u\n", t == lume::ScalarType::f32 ? "f32" : "i32", n);
        }
    }
}

}  // namespace

int main() {
    copies_for_all_four_representations();
    views_distinguish_scalar_type();
    moves_transfer_storage();
    executor_outputs_owned_results();
    executor_edge_lengths();
    return lume_test::finish("buffer_storage");
}

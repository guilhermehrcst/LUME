// E1 seam: the same verified program under the three execution plans
// (P5 materialize, P6 + last-use reuse, P7 + strict fusion). Every plan must
// match the independent oracle exactly, the executor's own stats must show the
// plan that was asked for, and the default policy must be the public executor.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <utility>
#include <vector>

#include "check.hpp"
#include "execution_policy.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"

namespace {

using lume::Buffer;
using lume::detail::ExecutionPolicy;
using lume::detail::ExecutionStats;

enum class Shape { single, final_left, final_right, inter_out, after_use, chain3, chain4 };
constexpr Shape kShapes[] = {Shape::single,    Shape::final_left, Shape::final_right, Shape::inter_out,
                             Shape::after_use, Shape::chain3,     Shape::chain4};
const char* name(Shape s) {
    switch (s) {
        case Shape::single: return "single";
        case Shape::final_left: return "final_left";
        case Shape::final_right: return "final_right";
        case Shape::inter_out: return "inter_out";
        case Shape::after_use: return "after_use";
        case Shape::chain3: return "chain3";
        case Shape::chain4: return "chain4";
    }
    return "?";
}
std::size_t inputs_of(Shape s) { return s == Shape::single ? 2 : s == Shape::chain3 ? 4 : s == Shape::chain4 ? 5 : 3; }

lume::VerifiedProgram build(Shape shape, lume::ScalarType t, std::uint32_t n) {
    lume::Program p;
    std::vector<lume::ValueId> v;
    for (std::size_t k = 0; k < inputs_of(shape); ++k) v.push_back(p.input(t, n));
    switch (shape) {
        case Shape::single: p.output(p.add(v[0], v[1])); break;
        case Shape::final_left: p.output(p.add(p.add(v[0], v[1]), v[2])); break;
        case Shape::final_right: p.output(p.add(v[2], p.add(v[0], v[1]))); break;
        case Shape::inter_out: {
            const auto d = p.add(v[0], v[1]);
            p.output(d);
            p.output(p.add(d, v[2]));
            break;
        }
        case Shape::after_use: {
            const auto d = p.add(v[0], v[1]);
            const auto e = p.add(d, v[2]);
            p.output(d);
            p.output(e);
            break;
        }
        case Shape::chain3: p.output(p.add(p.add(p.add(v[0], v[1]), v[2]), v[3])); break;
        case Shape::chain4: p.output(p.add(p.add(p.add(p.add(v[0], v[1]), v[2]), v[3]), v[4])); break;
    }
    lume::VerifyResult r = lume::verify(std::move(p));
    if (!r.ok()) std::abort();
    return std::move(*r.program);
}

// Expected outputs, independent of the executor (oracle adds, left to right).
template <class T>
std::vector<std::vector<T>> expected(Shape s, const std::vector<std::vector<T>>& in) {
    const auto add = [](const std::vector<T>& x, const std::vector<T>& y) {
        std::vector<T> out(x.size());
        lume_oracle::native_add(x, y, out);
        return out;
    };
    switch (s) {
        case Shape::single: return {add(in[0], in[1])};
        case Shape::final_left: return {add(add(in[0], in[1]), in[2])};
        case Shape::final_right: return {add(in[2], add(in[0], in[1]))};
        case Shape::inter_out:
        case Shape::after_use: {
            const auto d = add(in[0], in[1]);
            return {d, add(d, in[2])};
        }
        case Shape::chain3: return {add(add(add(in[0], in[1]), in[2]), in[3])};
        case Shape::chain4: return {add(add(add(add(in[0], in[1]), in[2]), in[3]), in[4])};
    }
    return {};
}

bool same(const Buffer& b, const std::vector<float>& e) {
    const auto v = b.f32_view();
    return v && lume_oracle::exactly_equal(*v, e);
}
bool same(const Buffer& b, const std::vector<std::int32_t>& e) {
    const auto v = b.i32_view();
    return v && v->size() == e.size() && std::equal(v->begin(), v->end(), e.begin());
}

struct Want {
    std::uint32_t fresh, in_place, fused, moves, copies;
};
// Expected stats per shape for policies {P5, P6, P7}.
Want want(Shape s, int plan) {
    switch (s) {
        case Shape::single: return {1, 0, 0, 1, 0};
        case Shape::final_left:
        case Shape::final_right:
            return plan == 0 ? Want{2, 0, 0, 1, 0} : plan == 1 ? Want{1, 1, 0, 1, 0} : Want{1, 0, 1, 1, 0};
        case Shape::inter_out: return plan == 0 ? Want{2, 0, 0, 1, 1} : Want{1, 1, 0, 1, 1};
        case Shape::after_use: return {2, 0, 0, 2, 0};
        case Shape::chain3:
            return plan == 0 ? Want{3, 0, 0, 1, 0} : plan == 1 ? Want{1, 2, 0, 1, 0} : Want{1, 1, 1, 1, 0};
        case Shape::chain4:
            return plan == 0 ? Want{4, 0, 0, 1, 0} : plan == 1 ? Want{1, 3, 0, 1, 0} : Want{1, 2, 1, 1, 0};
    }
    return {};
}
constexpr ExecutionPolicy kPlans[] = {{false, false}, {true, false}, {true, true}};

template <class T>
std::vector<T> gen(std::mt19937_64& e, std::size_t n);
template <>
std::vector<float> gen<float>(std::mt19937_64& e, std::size_t n) { return lume_oracle::generate_f32(e, n); }
template <>
std::vector<std::int32_t> gen<std::int32_t>(std::mt19937_64& e, std::size_t n) { return lume_oracle::generate_i32(e, n); }

template <class T>
void run_all(lume::ScalarType type) {
    for (const std::uint32_t n : {1u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 33u, 4099u}) {
        for (const Shape shape : kShapes) {
            std::mt19937_64 engine(1000 + n);
            std::vector<std::vector<T>> in;
            std::vector<Buffer> bufs;
            for (std::size_t k = 0; k < inputs_of(shape); ++k) {
                in.push_back(gen<T>(engine, n));
                bufs.emplace_back(in.back());
            }
            const auto want_out = expected<T>(shape, in);
            const lume::VerifiedProgram vp = build(shape, type, n);
            const lume::ExecutionResult pub = lume::execute_cpu_reference(vp, bufs);
            for (int plan = 0; plan < 3; ++plan) {
                ExecutionStats st;
                const lume::ExecutionResult r = lume::detail::execute_cpu_reference_with_policy(vp, bufs, kPlans[plan], &st);
                if (!LUME_CHECK(r.ok() && r.outputs.size() == want_out.size())) continue;
                for (std::size_t o = 0; o < want_out.size(); ++o) {
                    if (!LUME_CHECK(same(r.outputs[o], want_out[o]))) {
                        std::fprintf(stderr, "  shape=%s n=%u plan=P%d output=%zu\n", name(shape), n, plan + 5, o);
                    }
                }
                const Want w = want(shape, plan);
                const bool stats_ok = st.fresh_results == w.fresh && st.in_place_results == w.in_place &&
                                      st.fused_pairs == w.fused && st.output_moves == w.moves && st.output_copies == w.copies;
                if (!LUME_CHECK(stats_ok)) {
                    std::fprintf(stderr, "  shape=%s plan=P%d got fresh=%u inplace=%u fused=%u moves=%u copies=%u\n", name(shape),
                                 plan + 5, st.fresh_results, st.in_place_results, st.fused_pairs, st.output_moves,
                                 st.output_copies);
                }
                // Inputs are never modified.
                for (std::size_t k = 0; k < in.size(); ++k) LUME_CHECK(same(bufs[k], in[k]));
            }
            // The default policy is the public executor (P7).
            ExecutionStats st7;
            const lume::ExecutionResult d = lume::detail::execute_cpu_reference_with_policy(vp, bufs, ExecutionPolicy{}, &st7);
            LUME_CHECK(d.ok() && pub.ok() && d.outputs.size() == pub.outputs.size());
            for (std::size_t o = 0; o < want_out.size() && d.ok() && pub.ok(); ++o) LUME_CHECK(same(d.outputs[o], want_out[o]));
        }
    }
}

// Errors are policy-independent: the same input mismatch fails before any plan runs.
void errors_do_not_depend_on_policy() {
    const lume::VerifiedProgram vp = build(Shape::final_left, lume::f32, 8);
    const std::vector<Buffer> bad{Buffer(std::vector<float>(8)), Buffer(std::vector<float>(7)), Buffer(std::vector<float>(8))};
    for (const ExecutionPolicy& p : kPlans) {
        const lume::ExecutionResult r = lume::detail::execute_cpu_reference_with_policy(vp, bad, p);
        LUME_CHECK(!r.ok() && r.outputs.empty() &&
                   r.error->code == lume::ExecutionErrorCode::input_length_mismatch);
    }
}

}  // namespace

int main() {
    run_all<float>(lume::f32);
    run_all<std::int32_t>(lume::i32);
    errors_do_not_depend_on_policy();
    return lume_test::finish("policy");
}

// Lume M5: intermediate materialization baseline.
//
// Observational only; nothing here changes how Lume executes. For the chain
//   D = A + B; E = D + C
// it times the unmodified M4 executor, native fused and two-pass loops, and
// OwnedArray replicas of the executor's data path, so that interpretation,
// arithmetic and intermediate materialization can be separated. See
// docs/m5-intermediate-materialization.md.
//
// usage: lume_bench_intermediate_materialization [sizes=1,256,...] [seed=42]
//                                                [warmup=5] [iterations=51]
//                                                [order=forward|reverse]
//
// Output: one line per (N, component) of space-separated key=value fields.
//
// Methodology is the M1 harness (copied, not shared, so the M1-M4 benchmark
// stays byte-identical): a sample is one steady_clock interval around K
// repetitions (K = 1000 for N < 4096, else 1); results live in holders until
// the interval ends and are checked against the oracle outside it; minor page
// faults (getrusage) are read just outside each interval.

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#define LUME_HAVE_GETRUSAGE 1
#endif

#include "input_validation.hpp"
#include "lume/ir/program.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/runtime/owned_array.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"

#ifndef LUME_BUILD_CONFIG
#define LUME_BUILD_CONFIG "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t minor_faults() {
#ifdef LUME_HAVE_GETRUSAGE
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) return static_cast<std::int64_t>(usage.ru_minflt);
#endif
    return -1;
}

struct Config {
    std::vector<std::uint32_t> sizes{1, 256, 4096, 65536, 1048576, 4194304};
    std::uint64_t seed = 42;
    std::uint32_t warmup = 5;
    std::uint32_t iterations = 51;
    bool reverse = false;
};

struct Summary {
    double median = 0, min = 0, max = 0;  // ns per op
    double faults = 0;                    // median minor faults per op; -1 if unavailable
};

double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Setup and check run untimed around each interval; `run` is passed as a
// template argument so the timed loop has no indirect calls.
template <class Setup, class Run, class Check>
std::optional<Summary> measure(const Config& cfg, std::size_t batch, Setup&& setup, Run&& run, Check&& check) {
    std::vector<double> ns;
    std::vector<double> faults;
    for (std::uint32_t s = 0; s < cfg.warmup + cfg.iterations; ++s) {
        setup();
        const std::int64_t f0 = minor_faults();
        const auto t0 = Clock::now();
        for (std::size_t k = 0; k < batch; ++k) run(k);
        const auto t1 = Clock::now();
        const std::int64_t f1 = minor_faults();
        if (!check()) return std::nullopt;
        if (s < cfg.warmup) continue;
        const auto kd = static_cast<double>(batch);
        ns.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / kd);
        faults.push_back(f0 < 0 ? -1.0 : static_cast<double>(f1 - f0) / kd);
    }
    Summary out;
    out.median = median_of(ns);
    out.min = *std::min_element(ns.begin(), ns.end());
    out.max = *std::max_element(ns.begin(), ns.end());
    out.faults = median_of(faults);
    return out;
}

void print_summary(std::size_t n, const char* name, std::size_t batch, double model_bytes_per_element,
                   const Summary& s) {
    std::printf("n=%zu component=%s batch=%zu median_ns=%.1f min_ns=%.1f max_ns=%.1f ns_per_element=%.4f "
                "minflt_per_op=%.2f",
                n, name, batch, s.median, s.min, s.max, s.median / static_cast<double>(n), s.faults);
    if (model_bytes_per_element > 0) {
        const double bytes = model_bytes_per_element * static_cast<double>(n);
        std::printf(" model_bytes=%.0f model_gbps=%.2f", bytes, bytes / s.median);
    }
    std::printf(" correctness=exact\n");
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t v = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const auto d = static_cast<std::uint64_t>(ch - '0');
        if (v > (std::numeric_limits<std::uint64_t>::max() - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

bool parse_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto eq = arg.find('=');
        if (eq == std::string_view::npos) return false;
        const std::string_view key = arg.substr(0, eq);
        const std::string_view value = arg.substr(eq + 1);
        std::uint64_t v = 0;
        if (key == "sizes") {
            cfg.sizes.clear();
            std::size_t start = 0;
            while (start <= value.size()) {
                const auto comma = value.find(',', start);
                const auto item = value.substr(start, comma == std::string_view::npos ? value.npos : comma - start);
                if (!parse_u64(item, v) || v == 0 || v >= std::numeric_limits<std::uint32_t>::max()) return false;
                cfg.sizes.push_back(static_cast<std::uint32_t>(v));
                if (comma == std::string_view::npos) break;
                start = comma + 1;
            }
        } else if (key == "seed" && parse_u64(value, v)) {
            cfg.seed = v;
        } else if (key == "warmup" && parse_u64(value, v) && v <= 1000) {
            cfg.warmup = static_cast<std::uint32_t>(v);
        } else if (key == "iterations" && parse_u64(value, v) && v >= 1 && v <= 100000) {
            cfg.iterations = static_cast<std::uint32_t>(v);
        } else if (key == "order" && (value == "forward" || value == "reverse")) {
            cfg.reverse = value == "reverse";
        } else {
            return false;
        }
    }
    return !cfg.sizes.empty();
}

std::string compiler_id() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

// Repetitions per interval for operations whose cost scales with N. Large N
// uses K = 1 so allocator behavior matches one executor call at a time.
std::size_t batch_for(std::uint32_t n) { return n >= 4096 ? 1 : 1000; }
constexpr std::size_t fixed_batch = 1000;  // for N-independent operations

// ---- Native kernels (straightforward optimized code; explicit (a + b) + c) ----

void native_fused(std::span<const float> a, std::span<const float> b, std::span<const float> c,
                  std::span<float> e) {
    for (std::size_t i = 0; i < e.size(); ++i) e[i] = (a[i] + b[i]) + c[i];
}

void native_two_pass(std::span<const float> a, std::span<const float> b, std::span<const float> c,
                     std::span<float> d, std::span<float> e) {
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = a[i] + b[i];
    for (std::size_t i = 0; i < e.size(); ++i) e[i] = d[i] + c[i];
}

void native_fused_dual(std::span<const float> a, std::span<const float> b, std::span<const float> c,
                       std::span<float> d_out, std::span<float> e) {
    for (std::size_t i = 0; i < e.size(); ++i) {
        const float d = a[i] + b[i];
        d_out[i] = d;
        e[i] = d + c[i];
    }
}

// ---- OwnedArray kernels (M4 storage, the executor's add loop shape) ----

void owned_add(std::span<const float> x, std::span<const float> y, lume::OwnedArray<float>& out) {
    for (std::size_t i = 0; i < x.size(); ++i) out[i] = x[i] + y[i];
}

void owned_fused(std::span<const float> a, std::span<const float> b, std::span<const float> c,
                 lume::OwnedArray<float>& e) {
    for (std::size_t i = 0; i < a.size(); ++i) e[i] = (a[i] + b[i]) + c[i];
}

// ---- The Lume programs under test ----

enum class Chain { single_add, final_only, intermediate_output, output_after_use };

lume::VerifiedProgram build(Chain kind, std::uint32_t n) {
    lume::Program p;
    const auto a = p.input(lume::f32, n);
    const auto b = p.input(lume::f32, n);
    if (kind == Chain::single_add) {
        p.output(p.add(a, b));  // D = A + B; output D
    } else {
        const auto c = p.input(lume::f32, n);
        const auto d = p.add(a, b);
        if (kind == Chain::intermediate_output) p.output(d);  // copy: D is read below
        const auto e = p.add(d, c);
        if (kind == Chain::output_after_use) p.output(d);  // move: last use of D
        p.output(e);
    }
    lume::VerifyResult r = lume::verify(std::move(p));
    if (!r.ok()) std::abort();
    return std::move(*r.program);
}

bool run_size(const Config& cfg, std::uint32_t n_u32) {
    const std::size_t n = n_u32;

    // Deterministic inputs: A, B, C from one stream (seed 42 by default).
    std::mt19937_64 engine(cfg.seed);
    const std::vector<float> a = lume_oracle::generate_f32(engine, n);
    const std::vector<float> b = lume_oracle::generate_f32(engine, n);
    const std::vector<float> c = lume_oracle::generate_f32(engine, n);

    // Independent oracle: D = A + B, then E = D + C, with the oracle's own loop.
    std::vector<float> expected_d(n);
    std::vector<float> expected_e(n);
    lume_oracle::native_add(a, b, expected_d);
    lume_oracle::native_add(expected_d, c, expected_e);

    const std::vector<lume::Buffer> inputs{lume::Buffer(a), lume::Buffer(b), lume::Buffer(c)};
    const std::span<const lume::Buffer> inputs_ab(inputs.data(), 2);
    const lume::VerifiedProgram single = build(Chain::single_add, n_u32);
    const lume::VerifiedProgram final_only = build(Chain::final_only, n_u32);
    const lume::VerifiedProgram with_d = build(Chain::intermediate_output, n_u32);
    const lume::VerifiedProgram after_use = build(Chain::output_after_use, n_u32);

    const std::size_t kb = batch_for(n_u32);
    std::vector<std::vector<float>> dv(kb);
    std::vector<std::vector<float>> ev(kb);
    std::vector<lume::OwnedArray<float>> da(kb);
    std::vector<lume::OwnedArray<float>> ea(kb);
    std::vector<lume::ExecutionResult> results;
    results.reserve(kb);

    bool preallocated = false;
    const auto preallocate_once = [&] {
        if (preallocated) return;
        for (std::size_t k = 0; k < kb; ++k) {
            dv[k].assign(n, 0.0f);
            ev[k].assign(n, 0.0f);
        }
        preallocated = true;
    };
    const auto reset = [&] {
        for (auto& v : dv) std::vector<float>().swap(v);
        for (auto& v : ev) std::vector<float>().swap(v);
        for (auto& x : da) x = lume::OwnedArray<float>();
        for (auto& x : ea) x = lume::OwnedArray<float>();
        results.clear();
        preallocated = false;
    };
    const auto release_owned = [&] {
        for (auto& x : da) x = lume::OwnedArray<float>();
        for (auto& x : ea) x = lume::OwnedArray<float>();
    };
    const auto check_vec = [&](const std::vector<std::vector<float>>& h, const std::vector<float>& want) {
        for (std::size_t k = 0; k < kb; ++k) {
            if (!lume_oracle::exactly_equal(h[k], want)) return false;
        }
        return true;
    };
    const auto check_owned = [&](const std::vector<lume::OwnedArray<float>>& h, const std::vector<float>& want) {
        for (std::size_t k = 0; k < kb; ++k) {
            if (!lume_oracle::exactly_equal(h[k].view(), want)) return false;
        }
        return true;
    };
    // Every call must succeed and return exactly `want`, in order.
    const auto check_results = [&](const std::vector<const std::vector<float>*>& want) {
        if (results.size() != kb) return false;
        for (const auto& r : results) {
            if (!r.ok() || r.outputs.size() != want.size()) return false;
            for (std::size_t o = 0; o < want.size(); ++o) {
                const auto view = r.outputs[o].f32_view();
                if (!view || !lume_oracle::exactly_equal(*view, *want[o])) return false;
            }
        }
        return true;
    };
    const auto run_lume = [&](const lume::VerifiedProgram& vp, std::span<const lume::Buffer> in) {
        return measure(
            cfg, kb, [&] { results.clear(); },
            [&](std::size_t) { results.push_back(lume::execute_cpu_reference(vp, in)); },
            [&] {
                if (&vp == &single) return check_results({&expected_d});
                if (&vp == &final_only) return check_results({&expected_e});
                return check_results({&expected_d, &expected_e});  // with_d and after_use
            });
    };

    struct Entry {
        const char* name;
        std::size_t batch;
        double model_bytes_per_element;  // minimum user-level payload model; 0 when N-independent
        std::function<std::optional<Summary>()> measure;
    };
    std::vector<Entry> entries{
        {"timer_overhead", 1, 0, [&] { return measure(cfg, 1, [] {}, [](std::size_t) {}, [] { return true; }); }},
        {"native_fused_preallocated", kb, 16,
         [&] {
             return measure(
                 cfg, kb, preallocate_once, [&](std::size_t k) { native_fused(a, b, c, ev[k]); },
                 [&] { return check_vec(ev, expected_e); });
         }},
        {"native_two_pass_preallocated", kb, 24,
         [&] {
             return measure(
                 cfg, kb, preallocate_once, [&](std::size_t k) { native_two_pass(a, b, c, dv[k], ev[k]); },
                 [&] { return check_vec(dv, expected_d) && check_vec(ev, expected_e); });
         }},
        {"native_fused_dual_output", kb, 20,
         [&] {
             return measure(
                 cfg, kb, preallocate_once, [&](std::size_t k) { native_fused_dual(a, b, c, dv[k], ev[k]); },
                 [&] { return check_vec(dv, expected_d) && check_vec(ev, expected_e); });
         }},
        {"owned_fused_allocate_plus_compute", kb, 16,
         [&] {
             return measure(
                 cfg, kb, release_owned,
                 [&](std::size_t k) {
                     auto e = lume::OwnedArray<float>::for_overwrite(n);
                     owned_fused(a, b, c, e);
                     ea[k] = std::move(e);
                 },
                 [&] { return check_owned(ea, expected_e); });
         }},
        {"owned_two_pass_materialized", kb, 24,
         [&] {
             return measure(
                 cfg, kb, release_owned,
                 [&](std::size_t k) {
                     // The executor's data path without interpretation: allocate
                     // D, D = A + B, allocate E, E = D + C. D and E both stay
                     // alive until the interval ends.
                     auto d = lume::OwnedArray<float>::for_overwrite(n);
                     owned_add(a, b, d);
                     auto e = lume::OwnedArray<float>::for_overwrite(n);
                     owned_add(d.view(), c, e);
                     da[k] = std::move(d);
                     ea[k] = std::move(e);
                 },
                 [&] { return check_owned(da, expected_d) && check_owned(ea, expected_e); });
         }},
        {"input_validation_chain", fixed_batch, 0,
         [&] {
             std::size_t errors = 0;
             return measure(
                 cfg, fixed_batch, [&] { errors = 0; },
                 [&](std::size_t) {
                     if (lume::detail::validate_inputs(final_only.program().storage(), inputs)) ++errors;
                 },
                 [&] { return errors == 0; });
         }},
        {"lume_single_add_control", kb, 12, [&] { return run_lume(single, inputs_ab); }},
        {"lume_chain_final_only", kb, 24, [&] { return run_lume(final_only, inputs); }},
        {"lume_chain_intermediate_output", kb, 32, [&] { return run_lume(with_d, inputs); }},
        {"lume_chain_output_after_use", kb, 24, [&] { return run_lume(after_use, inputs); }},
    };
    if (cfg.reverse) std::reverse(entries.begin(), entries.end());

    for (const Entry& e : entries) {
        reset();
        const std::optional<Summary> s = e.measure();
        if (!s) {
            std::printf("n=%zu component=%s correctness=MISMATCH\n", n, e.name);
            return false;
        }
        print_summary(n, e.name, e.batch, e.model_bytes_per_element, *s);
    }
    std::printf("n=%zu checksum_d_fnv1a=0x%016" PRIx64 " checksum_e_fnv1a=0x%016" PRIx64 "\n", n,
                lume_oracle::fnv1a(expected_d), lume_oracle::fnv1a(expected_e));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        std::fprintf(stderr,
                     "usage: %s [sizes=1,256,...] [seed=42] [warmup=5] [iterations=51] [order=forward|reverse]\n",
                     argv[0]);
        return 2;
    }
    std::printf("lume_benchmark=intermediate_materialization\n");
    std::printf("workload=D=A+B;E=D+C dtype=f32 seed=%" PRIu64 " warmup=%u iterations=%u order=%s\n", cfg.seed,
                static_cast<unsigned>(cfg.warmup), static_cast<unsigned>(cfg.iterations),
                cfg.reverse ? "reverse" : "forward");
    std::printf("compiler=%s\nbuild_config=%s\n", compiler_id().c_str(), LUME_BUILD_CONFIG);
    std::printf("statistic=median_per_op_over_samples page_faults=%s\n",
                minor_faults() < 0 ? "unavailable" : "getrusage_ru_minflt");
    for (const std::uint32_t n : cfg.sizes) {
        if (!run_size(cfg, n)) return 1;
    }
    return 0;
}

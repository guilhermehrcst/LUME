// Lume M1/M2: cost breakdown of the scalar reference executor for C = A + B.
//
// Times the executor built from this tree plus isolated operations equivalent
// to each executor stage, over several N. Nothing here changes how Lume
// executes. See docs/m1-executor-cost-breakdown.md and
// docs/m2-output-move-last-use.md.
//
// Component names from M1 keep their M1 definitions: `output_materialization`
// and `data_path_replica` are the output COPY path. M2 adds
// `output_move_materialization` and `data_path_move_replica` for the output
// MOVE path. M4 adds `owned_array_allocate_for_overwrite`,
// `owned_array_indexed_add`, `owned_array_allocate_plus_add` and
// `data_path_owned_array_replica` for the OwnedArray single-write result
// path. Only `lume_execution_total` follows the executor in this tree.
//
// usage: lume_bench_executor_breakdown [sizes=1,256,...] [seed=42] [warmup=5]
//                                      [iterations=51] [order=forward|reverse]
//
// Output: one line per (N, component) of space-separated key=value fields.
//
// Methodology:
// - A sample is one steady_clock interval around K back-to-back repetitions
//   of the measured operation; the reported per-op time is interval / K.
//   K > 1 only where one operation is too short to time on its own.
// - Anything a repetition produces is kept in pre-sized holders until the
//   interval ends, then checked (untimed) against the oracle. Results are
//   therefore observable and cannot be removed as dead code. Allocations that
//   produce nothing observable escape through a volatile pointer sink.
// - Minor page faults (getrusage) are read just outside each interval.

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

// Writing an address here makes the pointed-to allocation observable without
// making the data itself volatile.
const void* volatile g_sink = nullptr;

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

// The executor's add loop, written in the same form as add_buffers().
void executor_style_add(const std::vector<float>& a, const std::vector<float>& b, std::vector<float>& c) {
    for (std::size_t i = 0; i < c.size(); ++i) c[i] = a[i] + b[i];
}

// The M4 single-write kernel: indexed writes into storage that was not
// value-initialized. Every element is written before it is read.
void owned_indexed_add(std::span<const float> a, std::span<const float> b, lume::OwnedArray<float>& c) {
    for (std::size_t i = 0; i < a.size(); ++i) c[i] = a[i] + b[i];
}

bool run_size(const Config& cfg, std::uint32_t n_u32) {
    const std::size_t n = n_u32;

    // Deterministic inputs, as in lume_bench_vector_add: A then B from one stream.
    std::mt19937_64 engine(cfg.seed);
    const std::vector<float> a = lume_oracle::generate_f32(engine, n);
    const std::vector<float> b = lume_oracle::generate_f32(engine, n);
    std::vector<float> expected(n);
    lume_oracle::native_add(a, b, expected);

    const std::vector<lume::Buffer> inputs{lume::Buffer(a), lume::Buffer(b)};
    // Operands for the isolated kernels. Buffer no longer exposes std::vector
    // (M4), so the historical vector-form controls read the generator vectors
    // `a` and `b` (same values; a separate allocation from the executor's
    // input Buffers).
    const std::vector<float>& in_a = a;
    const std::vector<float>& in_b = b;

    lume::Program program;
    program.output(program.add(program.input(lume::f32, n_u32), program.input(lume::f32, n_u32)));
    lume::VerifyResult verified = lume::verify(std::move(program));
    if (!verified.ok()) return false;
    const lume::VerifiedProgram& vp = *verified.program;
    const lume::ProgramStorage& storage = vp.program().storage();
    const std::size_t value_count = storage.values.size();

    const std::size_t kb = batch_for(n_u32);
    const lume::Buffer source(expected);  // what an output copy reads

    // Holders keep produced results alive until the interval ends.
    std::vector<std::vector<float>> vecs(std::max(kb, fixed_batch));
    std::vector<std::vector<lume::Buffer>> outs(kb);
    std::vector<std::optional<lume::Buffer>> move_sources(kb);
    std::vector<lume::OwnedArray<float>> arrays(kb);
    std::vector<lume::ExecutionResult> results;
    results.reserve(kb);

    const auto no_setup = [] {};
    const auto always_ok = [] { return true; };
    bool preallocated = false;
    // C is allocated and first touched once per component, untimed, so every
    // "preallocated" sample sees the same state: C mapped and last written by
    // the previous sample.
    const auto preallocate_once = [&] {
        if (preallocated) return;
        for (std::size_t k = 0; k < kb; ++k) vecs[k].assign(n, 0.0f);
        preallocated = true;
    };
    const auto release_all = [&] {
        for (auto& v : vecs) std::vector<float>().swap(v);
        preallocated = false;
    };
    const auto vecs_equal_expected = [&] {
        for (std::size_t k = 0; k < kb; ++k) {
            if (!lume_oracle::exactly_equal(vecs[k], expected)) return false;
        }
        return true;
    };

    // Each entry measures one component and prints it; false on a correctness failure.
    struct Entry {
        const char* name;
        std::size_t batch;
        double model_bytes_per_element;  // minimum traffic of the stated model; 0 when N-independent
        std::function<std::optional<Summary>()> measure;
    };
    std::vector<Entry> entries{
        {"timer_overhead", 1, 0, [&] { return measure(cfg, 1, no_setup, [](std::size_t) {}, always_ok); }},
        {"native_add_preallocated", kb, 12,
         [&] {
             return measure(cfg, kb, preallocate_once, [&](std::size_t k) { lume_oracle::native_add(a, b, vecs[k]); },
                            vecs_equal_expected);
         }},
        {"add_loop_preallocated", kb, 12,
         [&] {
             return measure(cfg, kb, preallocate_once,
                            [&](std::size_t k) { executor_style_add(in_a, in_b, vecs[k]); }, vecs_equal_expected);
         }},
        {"input_validation", fixed_batch, 0,
         [&] {
             std::size_t errors = 0;
             return measure(
                 cfg, fixed_batch, [&] { errors = 0; },
                 [&](std::size_t) {
                     if (lume::detail::validate_inputs(storage, inputs)) ++errors;
                 },
                 [&] { return errors == 0; });
         }},
        {"executor_setup", fixed_batch, 0,
         [&] {
             return measure(
                 cfg, fixed_batch, no_setup,
                 [&](std::size_t) {
                     // Same construction as execute_cpu_reference: bound, owned and
                     // an empty ExecutionResult, all destroyed at scope exit.
                     std::vector<const lume::Buffer*> bound(value_count, nullptr);
                     std::vector<std::optional<lume::Buffer>> owned(value_count);
                     lume::ExecutionResult result;
                     g_sink = bound.data();
                     g_sink = owned.data();
                     g_sink = &result;
                 },
                 always_ok);
         }},
        {"last_use_analysis", fixed_batch, 0,
         [&] {
             std::size_t unused = 0;
             return measure(
                 cfg, fixed_batch, [&] { unused = 0; },
                 [&](std::size_t) {
                     // Same construction as execute_cpu_reference (M2): one table
                     // entry per value, one pass over every operand.
                     std::vector<lume::OperationId> last_use(value_count);
                     for (std::size_t i = 0; i < storage.operations.size(); ++i) {
                         for (const lume::ValueId operand : storage.operations[i].operands) {
                             if (operand.is_valid()) {
                                 last_use[operand.index()] = lume::OperationId{static_cast<std::uint32_t>(i)};
                             }
                         }
                     }
                     g_sink = last_use.data();
                     for (const lume::OperationId op : last_use) if (!op.is_valid()) ++unused;
                 },
                 // %0, %1 are read by the add and %2 by the output: nothing unused.
                 [&] { return unused == 0; });
         }},
        {"result_buffer_create", kb, 4,
         [&] {
             return measure(
                 cfg, kb, release_all, [&](std::size_t k) { vecs[k] = std::vector<float>(n); },
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (vecs[k].size() != n || vecs[k][0] != 0.0f || vecs[k][n - 1] != 0.0f) return false;
                     }
                     return true;
                 });
         }},
        {"result_buffer_release", kb, 0,
         [&] {
             return measure(
                 cfg, kb,
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) vecs[k] = std::vector<float>(n);
                 },
                 [&](std::size_t k) { vecs[k] = std::vector<float>(); },
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (vecs[k].capacity() != 0) return false;
                     }
                     return true;
                 });
         }},
        {"result_create_plus_add", kb, 16,
         [&] {
             return measure(
                 cfg, kb, release_all,
                 [&](std::size_t k) {
                     std::vector<float> c(n);
                     executor_style_add(in_a, in_b, c);
                     vecs[k] = std::move(c);
                 },
                 vecs_equal_expected);
         }},
        {"output_materialization", kb, 8,
         [&] {
             return measure(
                 cfg, kb,
                 [&] {
                     for (auto& o : outs) std::vector<lume::Buffer>().swap(o);
                 },
                 [&](std::size_t k) { outs[k].push_back(source); },  // as result.outputs.push_back(*value)
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (outs[k].size() != 1 || !lume_oracle::exactly_equal(*outs[k][0].f32_view(), expected)) {
                             return false;
                         }
                     }
                     return true;
                 });
         }},
        {"validation_message_replica", fixed_batch, 0,
         [&] {
             std::size_t chars = 0;
             return measure(
                 cfg, fixed_batch, [&] { chars = 0; },
                 [&](std::size_t) {
                     // The `where` string validate_inputs builds for every input,
                     // even when the input is valid.
                     for (std::uint32_t k = 0; k < 2; ++k) {
                         const std::string where =
                             "input " + std::to_string(k) + " (%" + std::to_string(k) + ")";
                         g_sink = where.data();
                         chars += where.size();
                     }
                 },
                 [&] { return chars == fixed_batch * 24; });
         }},
        {"data_path_replica", kb, 24,
         [&] {
             return measure(
                 cfg, kb,
                 [&] {
                     for (auto& o : outs) std::vector<lume::Buffer>().swap(o);
                 },
                 [&](std::size_t k) {
                     // The executor's allocations and copies in the same order and
                     // with the same lifetimes, without validation or
                     // interpretation: create C, add, wrap, copy into a fresh
                     // outputs vector, free C. The output lives past the interval.
                     std::vector<float> c(n);
                     executor_style_add(in_a, in_b, c);
                     const lume::Buffer value(std::move(c));
                     std::vector<lume::Buffer> outputs;
                     outputs.push_back(value);
                     outs[k] = std::move(outputs);
                 },
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (outs[k].size() != 1 || !lume_oracle::exactly_equal(*outs[k][0].f32_view(), expected)) {
                             return false;
                         }
                     }
                     return true;
                 });
         }},
        {"output_move_materialization", kb, 0,
         [&] {
             return measure(
                 cfg, kb,
                 [&] {
                     // Untimed: one N-sized owned source per repetition, as the
                     // executor holds its add result in an optional<Buffer> slot.
                     for (auto& o : outs) std::vector<lume::Buffer>().swap(o);
                     for (std::size_t k = 0; k < kb; ++k) move_sources[k].emplace(expected);
                 },
                 [&](std::size_t k) {
                     // As the executor's final-use output: move into a fresh
                     // outputs vector, then reset the slot.
                     outs[k].push_back(std::move(*move_sources[k]));
                     move_sources[k].reset();
                 },
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (move_sources[k].has_value() || outs[k].size() != 1 ||
                             !lume_oracle::exactly_equal(*outs[k][0].f32_view(), expected)) {
                             return false;
                         }
                     }
                     return true;
                 });
         }},
        {"data_path_move_replica", kb, 16,
         [&] {
             return measure(
                 cfg, kb,
                 [&] {
                     for (auto& o : outs) std::vector<lume::Buffer>().swap(o);
                 },
                 [&](std::size_t k) {
                     // The M2 executor's allocations and transfers in the same
                     // order and with the same lifetimes, without validation or
                     // interpretation: create C, add, wrap in an owned slot, move
                     // into a fresh outputs vector. The output (C itself) lives
                     // past the interval.
                     std::vector<float> c(n);
                     executor_style_add(in_a, in_b, c);
                     std::optional<lume::Buffer> slot(lume::Buffer(std::move(c)));
                     std::vector<lume::Buffer> outputs;
                     outputs.push_back(std::move(*slot));
                     slot.reset();
                     outs[k] = std::move(outputs);
                 },
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (outs[k].size() != 1 || !lume_oracle::exactly_equal(*outs[k][0].f32_view(), expected)) {
                             return false;
                         }
                     }
                     return true;
                 });
         }},
        {"owned_array_allocate_for_overwrite", kb, 0,
         [&] {
             return measure(
                 cfg, kb,
                 [&] {
                     for (auto& x : arrays) x = lume::OwnedArray<float>();
                 },
                 // Allocation only: no element is written (or read).
                 [&](std::size_t k) { arrays[k] = lume::OwnedArray<float>::for_overwrite(n); },
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (arrays[k].size() != n || arrays[k].data() == nullptr) return false;
                     }
                     return true;
                 });
         }},
        {"owned_array_indexed_add", kb, 12,
         [&] {
             bool allocated = false;
             return measure(
                 cfg, kb,
                 [&] {
                     // Untimed, once: storage exists; the timed kernel writes every
                     // element before the check reads any.
                     if (allocated) return;
                     for (std::size_t k = 0; k < kb; ++k) arrays[k] = lume::OwnedArray<float>::for_overwrite(n);
                     allocated = true;
                 },
                 [&](std::size_t k) { owned_indexed_add(in_a, in_b, arrays[k]); },
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (!lume_oracle::exactly_equal(arrays[k].view(), expected)) return false;
                     }
                     return true;
                 });
         }},
        {"owned_array_allocate_plus_add", kb, 12,
         [&] {
             return measure(
                 cfg, kb,
                 [&] {
                     for (auto& x : arrays) x = lume::OwnedArray<float>();
                 },
                 [&](std::size_t k) {
                     auto c = lume::OwnedArray<float>::for_overwrite(n);
                     owned_indexed_add(in_a, in_b, c);
                     arrays[k] = std::move(c);
                 },
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (!lume_oracle::exactly_equal(arrays[k].view(), expected)) return false;
                     }
                     return true;
                 });
         }},
        {"data_path_owned_array_replica", kb, 12,
         [&] {
             return measure(
                 cfg, kb,
                 [&] {
                     for (auto& o : outs) std::vector<lume::Buffer>().swap(o);
                 },
                 [&](std::size_t k) {
                     // The M4 executor's data path without validation or
                     // interpretation: allocate C for overwrite, write each sum
                     // once, wrap in an owned slot, move into a fresh outputs
                     // vector. The output lives past the interval.
                     auto c = lume::OwnedArray<float>::for_overwrite(n);
                     owned_indexed_add(in_a, in_b, c);
                     std::optional<lume::Buffer> slot(lume::Buffer(std::move(c)));
                     std::vector<lume::Buffer> outputs;
                     outputs.push_back(std::move(*slot));
                     slot.reset();
                     outs[k] = std::move(outputs);
                 },
                 [&] {
                     for (std::size_t k = 0; k < kb; ++k) {
                         if (outs[k].size() != 1 || !lume_oracle::exactly_equal(*outs[k][0].f32_view(), expected)) {
                             return false;
                         }
                     }
                     return true;
                 });
         }},
        // Model bytes follow the executor in this tree: since M4 the single
        // output of C = A + B is written once into storage that is not
        // value-initialized (12: read A, read B, write C) and moved (0 N-scaled
        // payload bytes). M2 was 16, M1 was 24.
        {"lume_execution_total", kb, 12,
         [&] {
             return measure(
                 cfg, kb, [&] { results.clear(); },
                 [&](std::size_t) { results.push_back(lume::execute_cpu_reference(vp, inputs)); },
                 [&] {
                     if (results.size() != kb) return false;
                     for (const auto& r : results) {
                         if (!r.ok() || r.outputs.size() != 1 ||
                             !lume_oracle::exactly_equal(*r.outputs[0].f32_view(), expected)) {
                             return false;
                         }
                     }
                     return true;
                 });
         }},
    };
    if (cfg.reverse) std::reverse(entries.begin(), entries.end());

    for (const Entry& e : entries) {
        release_all();
        for (auto& o : outs) std::vector<lume::Buffer>().swap(o);
        results.clear();
        const std::optional<Summary> s = e.measure();
        if (!s) {
            std::printf("n=%zu component=%s correctness=MISMATCH\n", n, e.name);
            return false;
        }
        print_summary(n, e.name, e.batch, e.model_bytes_per_element, *s);
    }
    std::printf("n=%zu checksum_fnv1a=0x%016" PRIx64 "\n", n, lume_oracle::fnv1a(expected));
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
    std::printf("lume_benchmark=executor_breakdown\n");
    std::printf("workload=C=A+B dtype=f32 seed=%" PRIu64 " warmup=%u iterations=%u order=%s\n", cfg.seed,
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

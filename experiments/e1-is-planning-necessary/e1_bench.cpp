// E1: is planning necessary? Measures every distinct execution plan (P5/P6/P7)
// of each program in each size x allocation-regime cell, interleaved, and
// writes one CSV row per timed sample. See docs/experiments/e1-is-planning-necessary.md
// (preregistration) for the design; this file implements it and decides nothing.
//
// usage: lume_e1_bench out=<raw.csv> plans=<plans.csv> replicate=<id>
//                      regimes=glibc_default,arena_cold,arena_warm
//                      [programs=all|a,b] [sizes=64,1024,...] [reps=15] [seed=42]
//                      [warmup=3] [max_n_large=<n>]
// glibc_t1 / glibc_t2 must be launched under the matching GLIBC_TUNABLES;
// glibc_default and the arena regimes under none (checked, fail closed).

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnu/libc-version.h>
#include <sys/resource.h>

#include "alloc_hooks.hpp"
#include "execution_policy.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"
#include "workspace.hpp"

#ifndef LUME_E1_COMPILER
#define LUME_E1_COMPILER "unknown"
#endif
#ifndef LUME_E1_FLAGS
#define LUME_E1_FLAGS "unknown"
#endif
#ifndef LUME_E1_GIT_SHA
#define LUME_E1_GIT_SHA "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using lume::Buffer;
using lume::detail::ExecutionPolicy;
using lume::detail::ExecutionStats;

// ---------------------------------------------------------------- programs

enum class Shape { single, final_left, final_right, inter_out, after_use, chain3, chain4 };

struct ProgramSpec {
    const char* id;
    Shape shape;
    lume::ScalarType type;
    bool include_64m;  // also run N = 64 Mi (memory and time)
};

const ProgramSpec kPrograms[] = {
    {"single", Shape::single, lume::f32, false},
    {"final_left", Shape::final_left, lume::f32, true},
    {"final_right", Shape::final_right, lume::f32, false},
    {"inter_out", Shape::inter_out, lume::f32, false},
    {"after_use", Shape::after_use, lume::f32, false},
    {"chain3", Shape::chain3, lume::f32, true},
    {"chain4", Shape::chain4, lume::f32, false},
    {"final_left_i32", Shape::final_left, lume::i32, false},
    {"chain3_i32", Shape::chain3, lume::i32, false},
};

std::size_t input_count(Shape s) { return s == Shape::single ? 2 : s == Shape::chain3 ? 4 : s == Shape::chain4 ? 5 : 3; }

lume::VerifiedProgram build(Shape shape, lume::ScalarType t, std::uint32_t n) {
    lume::Program p;
    std::vector<lume::ValueId> v;
    for (std::size_t k = 0; k < input_count(shape); ++k) v.push_back(p.input(t, n));
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

template <class T>
std::vector<std::vector<T>> expected_outputs(Shape s, const std::vector<std::vector<T>>& in) {
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
            auto d = add(in[0], in[1]);
            auto e = add(d, in[2]);
            return {std::move(d), std::move(e)};
        }
        case Shape::chain3: return {add(add(add(in[0], in[1]), in[2]), in[3])};
        case Shape::chain4: return {add(add(add(add(in[0], in[1]), in[2]), in[3]), in[4])};
    }
    return {};
}

bool matches(const Buffer& b, const std::vector<float>& e) {
    const auto v = b.f32_view();
    return v && lume_oracle::exactly_equal(*v, e);
}
bool matches(const Buffer& b, const std::vector<std::int32_t>& e) {
    const auto v = b.i32_view();
    return v && v->size() == e.size() && std::equal(v->begin(), v->end(), e.begin());
}

template <class T>
std::vector<T> generate(std::mt19937_64& engine, std::size_t n);
template <>
std::vector<float> generate<float>(std::mt19937_64& engine, std::size_t n) {
    return lume_oracle::generate_f32(engine, n);
}
template <>
std::vector<std::int32_t> generate<std::int32_t>(std::mt19937_64& engine, std::size_t n) {
    return lume_oracle::generate_i32(engine, n);
}

// ---------------------------------------------------------------- config

struct Config {
    std::string out;
    std::string plans_out;
    std::string replicate = "0";
    std::vector<std::string> regimes;
    std::vector<std::string> programs;  // empty = all
    std::vector<std::uint64_t> sizes{64, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216, 67108864};
    std::uint32_t reps = 15;
    std::uint32_t warmup = 3;
    std::uint64_t seed = 42;
};

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto comma = s.find(',', start);
        out.emplace_back(s.substr(start, comma == std::string_view::npos ? s.npos : comma - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

bool parse_args(int argc, char** argv, Config& c) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto eq = a.find('=');
        if (eq == std::string_view::npos) return false;
        const std::string key(a.substr(0, eq));
        const std::string val(a.substr(eq + 1));
        if (key == "out") c.out = val;
        else if (key == "plans") c.plans_out = val;
        else if (key == "replicate") c.replicate = val;
        else if (key == "regimes") c.regimes = split(val);
        else if (key == "programs") { if (val != "all") c.programs = split(val); }
        else if (key == "sizes") {
            c.sizes.clear();
            for (const auto& t : split(val)) c.sizes.push_back(std::strtoull(t.c_str(), nullptr, 10));
        } else if (key == "reps") c.reps = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "warmup") c.warmup = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "seed") c.seed = std::strtoull(val.c_str(), nullptr, 10);
        else return false;
    }
    return !c.out.empty() && !c.plans_out.empty() && !c.regimes.empty() && c.reps >= 1;
}

// Fail closed: the process-level allocator regime must be what the label says.
bool regime_environment_ok(const std::string& regime) {
    const char* env = std::getenv("GLIBC_TUNABLES");
    const std::string tun = env == nullptr ? "" : env;
    if (regime == "glibc_t1") {
        return tun == "glibc.malloc.trim_threshold=1073741824:glibc.malloc.mmap_threshold=33554432";
    }
    if (regime == "glibc_t2") return tun == "glibc.malloc.mmap_threshold=131072";
    if (regime == "glibc_default" || regime == "arena_cold" || regime == "arena_warm") return tun.empty();
    return false;
}

// ---------------------------------------------------------------- measurement

struct Counters {
    std::int64_t minflt, majflt;
    std::int64_t utime_ns, stime_ns;
};

Counters read_counters() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    const auto ns = [](const timeval& t) { return static_cast<std::int64_t>(t.tv_sec) * 1000000000 + t.tv_usec * 1000; };
    return {u.ru_minflt, u.ru_majflt, ns(u.ru_utime), ns(u.ru_stime)};
}

struct Plan {
    std::string label;                 // canonical label: lowest policy index in the group
    std::string group;                 // e.g. "P6=P7"
    ExecutionPolicy policy;
    ExecutionStats stats;              // from the untimed dry run
    std::uint64_t dry_result_allocs = 0;
    std::int64_t dry_peak_live = 0;    // result-sized bytes live at the peak of one call (outputs included)
    bool is_fixed = false;             // the plan the fixed policy executes
    bool is_twin = false;              // FIXED_TWIN: a second series of the fixed plan, not a strategy
};

struct Cell {
    const ProgramSpec* prog;
    std::uint32_t n;
    std::size_t k;                     // calls per timed interval
    std::string regime;
};

constexpr const char* kCsvHeader =
    "replicate,program,dtype,n,regime,plan,rep,order_pos,k,ns_per_call,minflt_per_call,majflt_per_call,"
    "utime_ns_per_call,stime_ns_per_call,result_allocs_per_call,result_alloc_bytes_per_call,arena_allocs_per_call,"
    "arena_fallbacks,table_overflows,peak_live_bytes_interval,fresh,in_place,fused,copies,moves,correct\n";

constexpr const char* kPlansHeader =
    "replicate,program,dtype,n,plan,group,is_fixed,is_twin,reuse,fuse,fresh,in_place,fused,copies,moves,loops,passes,"
    "payload_bytes_per_element,dry_result_allocs,dry_peak_live_bytes\n";

template <class T>
bool run_cell(const Config& cfg, const Cell& cell, std::FILE* csv, std::FILE* plans_csv) {
    const ProgramSpec& prog = *cell.prog;
    const std::size_t n = cell.n;
    const std::size_t result_bytes = n * sizeof(T);
    const char* dtype = prog.type == lume::f32 ? "f32" : "i32";

    // Inputs and the oracle's expected outputs (independent of the executor).
    std::mt19937_64 engine(cfg.seed);
    std::vector<std::vector<T>> in;
    for (std::size_t k = 0; k < input_count(prog.shape); ++k) in.push_back(generate<T>(engine, n));
    const std::vector<std::vector<T>> want = expected_outputs<T>(prog.shape, in);
    std::vector<Buffer> inputs;
    for (auto& v : in) inputs.emplace_back(std::move(v));
    in.clear();
    in.shrink_to_fit();

    const lume::VerifiedProgram vp = build(prog.shape, prog.type, cell.n);

    // Untimed dry runs: what each policy actually does, and its logical peak.
    const ExecutionPolicy policies[3] = {{false, false}, {true, false}, {true, true}};
    std::vector<Plan> plans;
    for (int pi = 0; pi < 3; ++pi) {
        Plan p;
        p.policy = policies[pi];
        lume_e1::reset_counters();
        lume_e1::arm(nullptr, result_bytes);
        {
            const lume::ExecutionResult r = lume::detail::execute_cpu_reference_with_policy(vp, inputs, p.policy, &p.stats);
            lume_e1::disarm();
            const lume_e1::AllocCounters c = lume_e1::counters();
            p.dry_result_allocs = c.result_allocs;
            p.dry_peak_live = c.peak_live_bytes;
            if (!r.ok() || r.outputs.size() != want.size() || c.table_overflows != 0) return false;
            for (std::size_t o = 0; o < want.size(); ++o) {
                if (!matches(r.outputs[o], want[o])) return false;
            }
        }
        // Allocation accounting must agree with the executor's own stats.
        if (p.dry_result_allocs != p.stats.fresh_results + p.stats.output_copies) {
            std::fprintf(stderr, "accounting mismatch %s n=%zu P%d: allocs=%llu stats=%u+%u\n", prog.id, n, pi + 5,
                         static_cast<unsigned long long>(p.dry_result_allocs), p.stats.fresh_results, p.stats.output_copies);
            return false;
        }
        const std::string label = "P" + std::to_string(pi + 5);
        bool merged = false;
        for (Plan& q : plans) {
            if (q.stats == p.stats) {
                q.group += "=" + label;
                if (pi == 2) q.is_fixed = true;
                merged = true;
            }
        }
        if (!merged) {
            p.label = label;
            p.group = label;
            p.is_fixed = pi == 2;
            plans.push_back(p);
        }
    }
    // The twin: a second, independent series of the fixed plan (noise calibration only).
    for (const Plan& q : plans) {
        if (q.is_fixed) {
            Plan twin = q;
            twin.label = "FIXED_TWIN";
            twin.group = "FIXED_TWIN";
            twin.is_fixed = false;
            twin.is_twin = true;
            plans.push_back(twin);
            break;
        }
    }

    // plans.csv (once per program x n x replicate; regime independent)
    if (cell.regime == cfg.regimes.front()) {
        for (const Plan& p : plans) {
            const std::uint32_t loops = p.stats.fresh_results + p.stats.in_place_results;
            const std::uint32_t passes = loops + p.stats.output_copies;
            const double payload = 12.0 * (loops - p.stats.fused_pairs) + 16.0 * p.stats.fused_pairs + 8.0 * p.stats.output_copies;
            std::fprintf(plans_csv, "%s,%s,%s,%zu,%s,%s,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%.0f,%llu,%lld\n",
                         cfg.replicate.c_str(), prog.id, dtype, n, p.label.c_str(), p.group.c_str(), p.is_fixed ? 1 : 0,
                         p.is_twin ? 1 : 0, p.policy.reuse ? 1 : 0, p.policy.fuse ? 1 : 0, p.stats.fresh_results,
                         p.stats.in_place_results, p.stats.fused_pairs, p.stats.output_copies, p.stats.output_moves, loops,
                         passes, payload, static_cast<unsigned long long>(p.dry_result_allocs),
                         static_cast<long long>(p.dry_peak_live));
        }
        std::fflush(plans_csv);
    }

    // Workspace for the arena regimes: room for K held results plus one call's temporaries.
    const bool arena = cell.regime == "arena_cold" || cell.regime == "arena_warm";
    std::optional<lume_e1::Workspace> ws;
    if (arena) {
        std::int64_t peak = 0;
        std::uint64_t allocs = 0;
        for (const Plan& p : plans) {
            peak = std::max(peak, p.dry_peak_live);
            allocs = std::max(allocs, p.dry_result_allocs);
        }
        const std::size_t per_call = static_cast<std::size_t>(peak) + (allocs + 2) * 2 * lume_e1::Workspace::kHeader;
        const std::size_t capacity = (cell.k * per_call) * 5 / 4 + (1u << 20);
        ws.emplace(capacity);
        if (!ws->valid()) return false;
    }
    lume_e1::Workspace* wsp = arena ? &*ws : nullptr;

    std::vector<lume::ExecutionResult> results;
    results.reserve(cell.k);

    const auto call_interval = [&](const Plan& p) {
        for (std::size_t k = 0; k < cell.k; ++k) {
            results.push_back(lume::detail::execute_cpu_reference_with_policy(vp, inputs, p.policy));
        }
    };
    const auto check_results = [&]() {
        if (results.size() != cell.k) return false;
        for (const auto& r : results) {
            if (!r.ok() || r.outputs.size() != want.size()) return false;
            for (std::size_t o = 0; o < want.size(); ++o) {
                if (!matches(r.outputs[o], want[o])) return false;
            }
        }
        return true;
    };
    const auto clear_results = [&] {
        results.clear();  // frees every result (counted blocks go back to their owner)
    };

    const auto prime = [&](const Plan& p) {
        if (arena) wsp->reset();
        lume_e1::arm(wsp, result_bytes);
        call_interval(p);
        lume_e1::disarm();
        clear_results();
    };

    // Warm-up: whole intervals per plan, untimed, unrecorded.
    for (std::uint32_t w = 0; w < cfg.warmup; ++w) {
        for (const Plan& p : plans) prime(p);
    }

    const std::size_t m = plans.size();
    for (std::uint32_t rep = 0; rep < cfg.reps; ++rep) {
        for (std::size_t pos = 0; pos < m; ++pos) {
            const Plan& p = plans[(pos + rep) % m];  // Latin-square rotation of the plan order
            prime(p);
            if (arena) {
                if (!wsp->reset()) return false;
                if (cell.regime == "arena_cold" && !wsp->release_pages()) return false;
            }
            lume_e1::reset_counters();
            lume_e1::arm(wsp, result_bytes);
            const Counters c0 = read_counters();
            const auto t0 = Clock::now();
            call_interval(p);
            const auto t1 = Clock::now();
            const Counters c1 = read_counters();
            lume_e1::disarm();
            const lume_e1::AllocCounters ac = lume_e1::counters();
            const bool ok = check_results();
            clear_results();
            const double kd = static_cast<double>(cell.k);
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / kd;
            std::fprintf(csv, "%s,%s,%s,%zu,%s,%s,%u,%zu,%zu,%.2f,%.4f,%.4f,%.1f,%.1f,%.3f,%.1f,%.3f,%llu,%llu,%lld,%u,%u,%u,%u,%u,%d\n",
                         cfg.replicate.c_str(), prog.id, dtype, n, cell.regime.c_str(), p.label.c_str(), rep,
                         pos, cell.k, ns, static_cast<double>(c1.minflt - c0.minflt) / kd,
                         static_cast<double>(c1.majflt - c0.majflt) / kd, static_cast<double>(c1.utime_ns - c0.utime_ns) / kd,
                         static_cast<double>(c1.stime_ns - c0.stime_ns) / kd, static_cast<double>(ac.result_allocs) / kd,
                         static_cast<double>(ac.result_alloc_bytes) / kd, static_cast<double>(ac.arena_allocs) / kd,
                         static_cast<unsigned long long>(ac.arena_fallbacks), static_cast<unsigned long long>(ac.table_overflows),
                         static_cast<long long>(ac.peak_live_bytes), p.stats.fresh_results, p.stats.in_place_results,
                         p.stats.fused_pairs, p.stats.output_copies, p.stats.output_moves, ok ? 1 : 0);
            if (!ok) {
                std::fflush(csv);
                std::fprintf(stderr, "CORRECTNESS FAILURE %s n=%zu regime=%s plan=%s\n", prog.id, n, cell.regime.c_str(),
                             p.label.c_str());
                return false;
            }
            if (arena && ac.arena_fallbacks != 0) {
                std::fflush(csv);
                std::fprintf(stderr, "workspace exhausted %s n=%zu regime=%s: cell invalid\n", prog.id, n, cell.regime.c_str());
                return false;
            }
        }
    }
    std::fflush(csv);
    if (ws) lume_e1::forget(&*ws);
    return true;
}

std::string read_first_line(const char* path) {
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return "unavailable";
    char buf[512] = {};
    if (std::fgets(buf, sizeof buf, f) == nullptr) buf[0] = 0;
    std::fclose(f);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

void write_metadata(const Config& cfg) {
    std::string path = cfg.out + ".meta";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) return;
    std::string cpu = "unavailable";
    if (std::FILE* ci = std::fopen("/proc/cpuinfo", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, ci) != nullptr) {
            if (std::strncmp(line, "model name", 10) == 0) {
                cpu = std::strchr(line, ':') != nullptr ? std::strchr(line, ':') + 2 : line;
                while (!cpu.empty() && cpu.back() == '\n') cpu.pop_back();
                break;
            }
        }
        std::fclose(ci);
    }
    const char* tun = std::getenv("GLIBC_TUNABLES");
    std::fprintf(f, "commit_at_configure=%s\ncompiler=%s\nflags=%s\nglibc=%s\nkernel=%s\ncpu=%s\nthp=%s\nGLIBC_TUNABLES=%s\n",
                 LUME_E1_GIT_SHA, LUME_E1_COMPILER, LUME_E1_FLAGS, gnu_get_libc_version(),
                 read_first_line("/proc/sys/kernel/osrelease").c_str(), cpu.c_str(),
                 read_first_line("/sys/kernel/mm/transparent_hugepage/enabled").c_str(), tun == nullptr ? "" : tun);
    std::fprintf(f, "meminfo_total=%s\nenvironment=virtual machine (KVM guest); no hardware performance counters\n",
                 read_first_line("/proc/meminfo").c_str());
    std::fprintf(f, "FLT_EVAL_METHOD=%d\nsizeof_optional_buffer=%zu\nreps=%u\nwarmup=%u\nseed=%llu\n", static_cast<int>(
#ifdef FLT_EVAL_METHOD
                 FLT_EVAL_METHOD
#else
                 -1
#endif
                 ), sizeof(std::optional<Buffer>), cfg.reps, cfg.warmup, static_cast<unsigned long long>(cfg.seed));
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        std::fprintf(stderr,
                     "usage: %s out=<raw.csv> plans=<plans.csv> replicate=<id> regimes=glibc_default,arena_cold,arena_warm "
                     "[programs=..] [sizes=..] [reps=15] [warmup=3] [seed=42]\n",
                     argv[0]);
        return 2;
    }
    for (const std::string& r : cfg.regimes) {
        if (!regime_environment_ok(r)) {
            std::fprintf(stderr, "regime %s does not match the process environment (GLIBC_TUNABLES)\n", r.c_str());
            return 2;
        }
    }
    write_metadata(cfg);
    std::FILE* csv = std::fopen(cfg.out.c_str(), "w");
    std::FILE* plans_csv = std::fopen(cfg.plans_out.c_str(), "w");
    if (csv == nullptr || plans_csv == nullptr) return 2;
    std::fputs(kCsvHeader, csv);
    std::fputs(kPlansHeader, plans_csv);

    for (const ProgramSpec& prog : kPrograms) {
        if (!cfg.programs.empty() && std::find(cfg.programs.begin(), cfg.programs.end(), prog.id) == cfg.programs.end()) continue;
        for (const std::uint64_t n : cfg.sizes) {
            if (n == 67108864 && !prog.include_64m) continue;
            for (const std::string& regime : cfg.regimes) {
                const Cell cell{&prog, static_cast<std::uint32_t>(n), n < 4096 ? std::size_t{1000} : std::size_t{1}, regime};
                const bool ok = prog.type == lume::f32 ? run_cell<float>(cfg, cell, csv, plans_csv)
                                                       : run_cell<std::int32_t>(cfg, cell, csv, plans_csv);
                if (!ok) {
                    std::fprintf(stderr, "cell failed: %s n=%llu regime=%s\n", prog.id, static_cast<unsigned long long>(n),
                                 regime.c_str());
                    return 1;
                }
                std::fprintf(stderr, "done %s n=%llu %s\n", prog.id, static_cast<unsigned long long>(n), regime.c_str());
            }
        }
    }
    std::fclose(csv);
    std::fclose(plans_csv);
    return 0;
}

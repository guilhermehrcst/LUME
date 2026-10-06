// E2: materialize vs recompute. For the program D = A + B, Rj = D + Cj
// (j = 1..k) measures strategy M (D computed once, last consumer reuses its
// storage) and strategy R (D never stored, each consumer runs (A + B) + C in
// one loop), plus M_TWIN (a second series of M, noise calibration only), in
// each dtype x fan-out x size x allocation-regime cell. One CSV row per timed
// sample. See docs/experiments/e2-materialize-vs-recompute.md (preregistration);
// this file implements it and decides nothing.
//
// usage: lume_e2_bench out=<raw.csv> plans=<strategies.csv> replicate=<id>
//                      regimes=arena_warm,arena_cold,glibc_default_fresh
//                      [dtypes=f32,i32] [ks=1,2,3,4,6,8] [sizes=64,...] [reps=15]
//                      [warmup=3] [seed=42]
// glibc_default_fresh must be launched as one process per cell (run_e2.sh).

#include <algorithm>
#include <bit>
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

#ifndef LUME_E2_COMPILER
#define LUME_E2_COMPILER "unknown"
#endif
#ifndef LUME_E2_FLAGS
#define LUME_E2_FLAGS "unknown"
#endif
#ifndef LUME_E2_GIT_SHA
#define LUME_E2_GIT_SHA "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using lume::Buffer;
using lume::detail::ExecutionPolicy;
using lume::detail::ExecutionStats;

lume::VerifiedProgram build(lume::ScalarType t, std::uint32_t n, std::size_t k) {
    lume::Program p;
    const auto a = p.input(t, n);
    const auto b = p.input(t, n);
    std::vector<lume::ValueId> c;
    for (std::size_t j = 0; j < k; ++j) c.push_back(p.input(t, n));
    const auto d = p.add(a, b);
    std::vector<lume::ValueId> r;
    for (std::size_t j = 0; j < k; ++j) r.push_back(p.add(d, c[j]));
    for (const auto& v : r) p.output(v);
    lume::VerifyResult v = lume::verify(std::move(p));
    if (!v.ok()) std::abort();
    return std::move(*v.program);
}

template <class T>
std::vector<T> oracle_add(const std::vector<T>& x, const std::vector<T>& y) {
    std::vector<T> out(x.size());
    lume_oracle::native_add(x, y, out);
    return out;
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

struct Config {
    std::string out;
    std::string plans_out;
    std::string replicate = "0";
    std::vector<std::string> regimes;
    std::vector<std::string> dtypes{"f32", "i32"};
    std::vector<std::uint64_t> ks{1, 2, 3, 4, 6, 8};
    std::vector<std::uint64_t> sizes{64, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216};
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

std::vector<std::uint64_t> to_numbers(const std::string& val) {
    std::vector<std::uint64_t> v;
    for (const auto& t : split(val)) v.push_back(std::strtoull(t.c_str(), nullptr, 10));
    return v;
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
        else if (key == "dtypes") c.dtypes = split(val);
        else if (key == "ks") c.ks = to_numbers(val);
        else if (key == "sizes") c.sizes = to_numbers(val);
        else if (key == "reps") c.reps = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "warmup") c.warmup = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "seed") c.seed = std::strtoull(val.c_str(), nullptr, 10);
        else return false;
    }
    return !c.out.empty() && !c.plans_out.empty() && !c.regimes.empty() && c.reps >= 1;
}

// Fail closed: every E2 regime runs under the default allocator tunables.
bool regime_environment_ok(const std::string& regime) {
    const char* env = std::getenv("GLIBC_TUNABLES");
    const std::string tun = env == nullptr ? "" : env;
    return (regime == "glibc_default_fresh" || regime == "arena_cold" || regime == "arena_warm") && tun.empty();
}

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

struct Strategy {
    std::string label;  // M, R, M_TWIN
    ExecutionPolicy policy;
    ExecutionStats stats;  // from the untimed dry run
    std::uint64_t dry_result_allocs = 0;
    std::int64_t dry_peak_live = 0;
    bool is_twin = false;
};

struct Cell {
    std::string dtype;
    std::size_t fan;  // k
    std::uint32_t n;
    std::size_t calls;  // calls per timed interval
    std::string regime;
};

constexpr const char* kCsvHeader =
    "replicate,dtype,fanout,n,regime,strategy,rep,order_pos,calls,ns_per_call,minflt_per_call,majflt_per_call,"
    "utime_ns_per_call,stime_ns_per_call,result_allocs_per_call,result_alloc_bytes_per_call,arena_allocs_per_call,"
    "arena_fallbacks,table_overflows,peak_live_bytes_interval,fresh,in_place,recomputed,elided,copies,moves,correct\n";

constexpr const char* kPlansHeader =
    "replicate,dtype,fanout,n,strategy,is_twin,fresh,in_place,recomputed,elided,copies,moves,loops,passes,"
    "payload_bytes_per_element,dry_result_allocs,dry_peak_live_bytes,bookkeeping_collision\n";

template <class T>
bool run_cell(const Config& cfg, const Cell& cell, std::FILE* csv, std::FILE* plans_csv) {
    const std::size_t n = cell.n;
    const std::size_t k = cell.fan;
    const std::size_t result_bytes = n * sizeof(T);
    const lume::ScalarType scalar = cell.dtype == "f32" ? lume::f32 : lume::i32;

    // Inputs A, B, C1..Ck and the oracle's outputs, independent of the executor.
    std::mt19937_64 engine(cfg.seed);
    std::vector<std::vector<T>> in;
    for (std::size_t q = 0; q < k + 2; ++q) in.push_back(generate<T>(engine, n));
    const std::vector<T> d = oracle_add(in[0], in[1]);
    std::vector<std::vector<T>> want;
    for (std::size_t j = 0; j < k; ++j) want.push_back(oracle_add(d, in[2 + j]));
    std::vector<Buffer> inputs;
    for (auto& v : in) inputs.emplace_back(std::move(v));
    in.clear();
    in.shrink_to_fit();

    const lume::VerifiedProgram vp = build(scalar, cell.n, k);

    const auto uk = static_cast<std::uint32_t>(k);
    // Known accounting collision: the executor's `outputs` vector grows by
    // doubling (capacity 1, 2, 4, ...), and a capacity whose byte size equals the
    // result size (e.g. 8 x sizeof(Buffer) = 256 B = 4 * 64 for k = 5..8 at
    // N = 64) is counted as a result-sized allocation. It is identical for M and
    // R, and is accounted for here instead of loosening the check.
    std::uint64_t bookkeeping_collision = 0;
    for (std::size_t cap = 1; cap < k * 2 && cap <= std::bit_ceil(k); cap *= 2) {
        if (cap * sizeof(Buffer) == result_bytes) ++bookkeeping_collision;
    }
    std::vector<Strategy> strategies;
    for (int si = 0; si < 2; ++si) {
        Strategy s;
        s.label = si == 0 ? "M" : "R";
        s.policy = si == 0 ? ExecutionPolicy{true, false, false} : ExecutionPolicy{true, false, true};
        lume_e1::reset_counters();
        lume_e1::arm(nullptr, result_bytes);
        {
            const lume::ExecutionResult r = lume::detail::execute_cpu_reference_with_policy(vp, inputs, s.policy, &s.stats);
            lume_e1::disarm();
            const lume_e1::AllocCounters c = lume_e1::counters();
            s.dry_result_allocs = c.result_allocs;
            s.dry_peak_live = c.peak_live_bytes;
            if (!r.ok() || r.outputs.size() != want.size() || c.table_overflows != 0) return false;
            for (std::size_t o = 0; o < want.size(); ++o) {
                if (!matches(r.outputs[o], want[o])) return false;
            }
        }
        // The seam must have done what the label says; the allocation hook must agree with the executor's own count.
        const ExecutionStats expect = si == 0 ? ExecutionStats{uk, 1, 0, uk, 0, 0, 0} : ExecutionStats{uk, 0, 0, uk, 0, uk, 1};
        if (!(s.stats == expect) || s.dry_result_allocs != s.stats.fresh_results + s.stats.output_copies + bookkeeping_collision) {
            std::fprintf(stderr, "strategy %s did not run as labelled (k=%zu n=%zu): fresh=%u inplace=%u rec=%u elided=%u allocs=%llu\n",
                         s.label.c_str(), k, n, s.stats.fresh_results, s.stats.in_place_results, s.stats.recomputed_consumers,
                         s.stats.elided_producers, static_cast<unsigned long long>(s.dry_result_allocs));
            return false;
        }
        strategies.push_back(s);
    }
    {
        Strategy twin = strategies[0];
        twin.label = "M_TWIN";
        twin.is_twin = true;
        strategies.push_back(twin);
    }

    if (cell.regime == cfg.regimes.front()) {
        for (const Strategy& s : strategies) {
            const std::uint32_t loops = s.stats.fresh_results + s.stats.in_place_results;
            const std::uint32_t passes = loops + s.stats.output_copies;
            const double payload = 12.0 * (loops - s.stats.recomputed_consumers) + 16.0 * s.stats.recomputed_consumers +
                                   8.0 * s.stats.output_copies;
            std::fprintf(plans_csv, "%s,%s,%zu,%zu,%s,%d,%u,%u,%u,%u,%u,%u,%u,%u,%.0f,%llu,%lld,%llu\n", cfg.replicate.c_str(),
                         cell.dtype.c_str(), k, n, s.label.c_str(), s.is_twin ? 1 : 0, s.stats.fresh_results,
                         s.stats.in_place_results, s.stats.recomputed_consumers, s.stats.elided_producers, s.stats.output_copies,
                         s.stats.output_moves, loops, passes, payload, static_cast<unsigned long long>(s.dry_result_allocs),
                         static_cast<long long>(s.dry_peak_live),
                         static_cast<unsigned long long>(bookkeeping_collision));
        }
        std::fflush(plans_csv);
    }

    const bool arena = cell.regime == "arena_cold" || cell.regime == "arena_warm";
    std::optional<lume_e1::Workspace> ws;
    if (arena) {
        std::int64_t peak = 0;
        std::uint64_t allocs = 0;
        for (const Strategy& s : strategies) {
            peak = std::max(peak, s.dry_peak_live);
            allocs = std::max(allocs, s.dry_result_allocs);
        }
        const std::size_t per_call = static_cast<std::size_t>(peak) + (allocs + 2) * 2 * lume_e1::Workspace::kHeader;
        const std::size_t capacity = (cell.calls * per_call) * 5 / 4 + (1u << 20);
        ws.emplace(capacity);
        if (!ws->valid()) return false;
    }
    lume_e1::Workspace* wsp = arena ? &*ws : nullptr;

    std::vector<lume::ExecutionResult> results;
    results.reserve(cell.calls);

    const auto call_interval = [&](const Strategy& s) {
        for (std::size_t c = 0; c < cell.calls; ++c) {
            results.push_back(lume::detail::execute_cpu_reference_with_policy(vp, inputs, s.policy));
        }
    };
    const auto check_results = [&]() {
        if (results.size() != cell.calls) return false;
        for (const auto& r : results) {
            if (!r.ok() || r.outputs.size() != want.size()) return false;
            for (std::size_t o = 0; o < want.size(); ++o) {
                if (!matches(r.outputs[o], want[o])) return false;
            }
        }
        return true;
    };
    const auto prime = [&](const Strategy& s) {
        if (arena) wsp->reset();
        lume_e1::arm(wsp, result_bytes);
        call_interval(s);
        lume_e1::disarm();
        results.clear();
    };

    for (std::uint32_t w = 0; w < cfg.warmup; ++w) {
        for (const Strategy& s : strategies) prime(s);
    }

    const std::size_t m = strategies.size();
    for (std::uint32_t rep = 0; rep < cfg.reps; ++rep) {
        for (std::size_t pos = 0; pos < m; ++pos) {
            const Strategy& s = strategies[(pos + rep) % m];  // Latin-square rotation
            prime(s);
            if (arena) {
                if (!wsp->reset()) return false;
                if (cell.regime == "arena_cold" && !wsp->release_pages()) return false;
            }
            lume_e1::reset_counters();
            lume_e1::arm(wsp, result_bytes);
            const Counters c0 = read_counters();
            const auto t0 = Clock::now();
            call_interval(s);
            const auto t1 = Clock::now();
            const Counters c1 = read_counters();
            lume_e1::disarm();
            const lume_e1::AllocCounters ac = lume_e1::counters();
            const bool ok = check_results();
            results.clear();
            const double kd = static_cast<double>(cell.calls);
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / kd;
            std::fprintf(csv, "%s,%s,%zu,%zu,%s,%s,%u,%zu,%zu,%.2f,%.4f,%.4f,%.1f,%.1f,%.3f,%.1f,%.3f,%llu,%llu,%lld,%u,%u,%u,%u,%u,%u,%d\n",
                         cfg.replicate.c_str(), cell.dtype.c_str(), k, n, cell.regime.c_str(), s.label.c_str(), rep, pos,
                         cell.calls, ns, static_cast<double>(c1.minflt - c0.minflt) / kd,
                         static_cast<double>(c1.majflt - c0.majflt) / kd, static_cast<double>(c1.utime_ns - c0.utime_ns) / kd,
                         static_cast<double>(c1.stime_ns - c0.stime_ns) / kd, static_cast<double>(ac.result_allocs) / kd,
                         static_cast<double>(ac.result_alloc_bytes) / kd, static_cast<double>(ac.arena_allocs) / kd,
                         static_cast<unsigned long long>(ac.arena_fallbacks), static_cast<unsigned long long>(ac.table_overflows),
                         static_cast<long long>(ac.peak_live_bytes), s.stats.fresh_results, s.stats.in_place_results,
                         s.stats.recomputed_consumers, s.stats.elided_producers, s.stats.output_copies, s.stats.output_moves,
                         ok ? 1 : 0);
            if (!ok) {
                std::fflush(csv);
                std::fprintf(stderr, "CORRECTNESS FAILURE %s k=%zu n=%zu regime=%s strategy=%s\n", cell.dtype.c_str(), k, n,
                             cell.regime.c_str(), s.label.c_str());
                return false;
            }
            if (ac.table_overflows != 0 || (arena && ac.arena_fallbacks != 0)) {
                std::fflush(csv);
                std::fprintf(stderr, "invalid cell (table overflow or workspace exhausted) %s k=%zu n=%zu regime=%s\n",
                             cell.dtype.c_str(), k, n, cell.regime.c_str());
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

std::string meminfo_line(const char* key) {
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (f == nullptr) return "unavailable";
    char line[256];
    std::string out = "unavailable";
    while (std::fgets(line, sizeof line, f) != nullptr) {
        if (std::strncmp(line, key, std::strlen(key)) == 0) {
            out = line;
            while (!out.empty() && out.back() == '\n') out.pop_back();
            break;
        }
    }
    std::fclose(f);
    return out;
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
                 LUME_E2_GIT_SHA, LUME_E2_COMPILER, LUME_E2_FLAGS, gnu_get_libc_version(),
                 read_first_line("/proc/sys/kernel/osrelease").c_str(), cpu.c_str(),
                 read_first_line("/sys/kernel/mm/transparent_hugepage/enabled").c_str(), tun == nullptr ? "" : tun);
    std::fprintf(f, "%s\n%s\n%s\nenvironment=virtual machine (KVM guest); no hardware performance counters\n",
                 meminfo_line("MemTotal").c_str(), meminfo_line("SwapTotal").c_str(), meminfo_line("SwapFree").c_str());
    std::fprintf(f, "FLT_EVAL_METHOD=%d\nreps=%u\nwarmup=%u\nseed=%llu\n", static_cast<int>(
#ifdef FLT_EVAL_METHOD
                 FLT_EVAL_METHOD
#else
                 -1
#endif
                 ), cfg.reps, cfg.warmup, static_cast<unsigned long long>(cfg.seed));
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        std::fprintf(stderr,
                     "usage: %s out=<raw.csv> plans=<strategies.csv> replicate=<id> regimes=arena_warm,arena_cold,glibc_default_fresh "
                     "[dtypes=..] [ks=..] [sizes=..] [reps=15] [warmup=3] [seed=42]\n",
                     argv[0]);
        return 2;
    }
    for (const std::string& r : cfg.regimes) {
        if (!regime_environment_ok(r)) {
            std::fprintf(stderr, "regime %s is unknown or does not match the process environment (GLIBC_TUNABLES)\n", r.c_str());
            return 2;
        }
    }
    write_metadata(cfg);
    std::FILE* csv = std::fopen(cfg.out.c_str(), "w");
    std::FILE* plans_csv = std::fopen(cfg.plans_out.c_str(), "w");
    if (csv == nullptr || plans_csv == nullptr) return 2;
    std::fputs(kCsvHeader, csv);
    std::fputs(kPlansHeader, plans_csv);

    for (const std::string& dtype : cfg.dtypes) {
        for (const std::uint64_t fan : cfg.ks) {
            for (const std::uint64_t n : cfg.sizes) {
                for (const std::string& regime : cfg.regimes) {
                    const Cell cell{dtype, static_cast<std::size_t>(fan), static_cast<std::uint32_t>(n),
                                    n < 4096 ? std::size_t{1000} : std::size_t{1}, regime};
                    const bool ok = dtype == "f32" ? run_cell<float>(cfg, cell, csv, plans_csv)
                                                   : run_cell<std::int32_t>(cfg, cell, csv, plans_csv);
                    if (!ok) {
                        std::fprintf(stderr, "cell failed: %s k=%llu n=%llu regime=%s\n", dtype.c_str(),
                                     static_cast<unsigned long long>(fan), static_cast<unsigned long long>(n), regime.c_str());
                        return 1;
                    }
                    std::fprintf(stderr, "done %s k=%llu n=%llu %s\n", dtype.c_str(), static_cast<unsigned long long>(fan),
                                 static_cast<unsigned long long>(n), regime.c_str());
                }
            }
        }
    }
    std::fclose(csv);
    std::fclose(plans_csv);
    return 0;
}

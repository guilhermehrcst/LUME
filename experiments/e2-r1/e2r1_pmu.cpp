// E2-R1 secondary PMU characterization (separate from the primary timing dataset).
// Runs the E2 protocol (same program, M and R policies, arena_warm workspace, untimed
// priming, oracle check outside the timed region) for a fixed subset of cells and reads
// hardware/software counters around each timed interval with perf_event_open, using the
// architecture-independent generic encodings. Events are probed, never assumed: an
// unsupported event is written as supported=0 with its errno. Timing taken here is
// perf-instrumented and must never be merged into the primary dataset.
//
// usage: lume_e2r1_pmu out=<pmu.csv> compiler=<label> replicate=<id> events=<name,name,..>
//                      [dtypes=f32,i32] [ks=1,2,3,4] [sizes=4096,65536,1048576,4194304]
//                      [strategies=M,R] [reps=15] [warmup=3] [seed=42]
//        lume_e2r1_pmu probe=1 out=<probe.csv>      (only report which events can be opened)

#include <algorithm>
#include <cerrno>
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

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "alloc_hooks.hpp"
#include "execution_policy.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"
#include "workspace.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using lume::Buffer;
using lume::detail::ExecutionPolicy;
using lume::detail::ExecutionStats;

// ------------------------------------------------------------------ events

struct EventDef {
    const char* name;
    std::uint32_t type;
    std::uint64_t config;
};

constexpr std::uint64_t cache_cfg(std::uint64_t id, std::uint64_t op, std::uint64_t result) {
    return id | (op << 8) | (result << 16);
}

const EventDef kEvents[] = {
    {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
    {"branches", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
    {"branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
    {"cache-references", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
    {"cache-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
    {"L1-dcache-loads", PERF_TYPE_HW_CACHE, cache_cfg(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
    {"L1-dcache-load-misses", PERF_TYPE_HW_CACHE, cache_cfg(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
    {"LLC-loads", PERF_TYPE_HW_CACHE, cache_cfg(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
    {"LLC-load-misses", PERF_TYPE_HW_CACHE, cache_cfg(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
    {"dTLB-load-misses", PERF_TYPE_HW_CACHE, cache_cfg(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
    {"page-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS},
    {"minor-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MIN},
    {"major-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MAJ},
};

struct Counter {
    const EventDef* def = nullptr;
    int fd = -1;
    int err = 0;
};

int open_event(const EventDef& d, int* err) {
    perf_event_attr a;
    std::memset(&a, 0, sizeof a);
    a.type = d.type;
    a.size = sizeof a;
    a.config = d.config;
    a.disabled = 1;
    a.exclude_kernel = 1;  // user-space only: works under perf_event_paranoid = 2
    a.exclude_hv = 1;
    a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    const long fd = syscall(SYS_perf_event_open, &a, 0, -1, -1, 0UL);
    if (fd < 0) {
        *err = errno;
        return -1;
    }
    return static_cast<int>(fd);
}

const EventDef* find_event(std::string_view name) {
    for (const EventDef& e : kEvents) {
        if (name == e.name) return &e;
    }
    return nullptr;
}

// ------------------------------------------------------------------ program (as in the primary harness)

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
std::vector<float> generate<float>(std::mt19937_64& engine, std::size_t n) { return lume_oracle::generate_f32(engine, n); }
template <>
std::vector<std::int32_t> generate<std::int32_t>(std::mt19937_64& engine, std::size_t n) { return lume_oracle::generate_i32(engine, n); }

// ------------------------------------------------------------------ config

struct Config {
    std::string out;
    std::string compiler = "unknown";
    std::string replicate = "0";
    std::vector<std::string> events;
    std::vector<std::string> dtypes{"f32", "i32"};
    std::vector<std::uint64_t> ks{1, 2, 3, 4};
    std::vector<std::uint64_t> sizes{4096, 65536, 1048576, 4194304};
    std::vector<std::string> strategies{"M", "R"};
    std::uint32_t reps = 15;
    std::uint32_t warmup = 3;
    std::uint64_t seed = 42;
    bool probe = false;
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
std::vector<std::uint64_t> to_numbers(const std::string& v) {
    std::vector<std::uint64_t> r;
    for (const auto& t : split(v)) r.push_back(std::strtoull(t.c_str(), nullptr, 10));
    return r;
}

bool parse_args(int argc, char** argv, Config& c) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto eq = a.find('=');
        if (eq == std::string_view::npos) return false;
        const std::string key(a.substr(0, eq)), val(a.substr(eq + 1));
        if (key == "out") c.out = val;
        else if (key == "compiler") c.compiler = val;
        else if (key == "replicate") c.replicate = val;
        else if (key == "events") c.events = split(val);
        else if (key == "dtypes") c.dtypes = split(val);
        else if (key == "ks") c.ks = to_numbers(val);
        else if (key == "sizes") c.sizes = to_numbers(val);
        else if (key == "strategies") c.strategies = split(val);
        else if (key == "reps") c.reps = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "warmup") c.warmup = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "seed") c.seed = std::strtoull(val.c_str(), nullptr, 10);
        else if (key == "probe") c.probe = val == "1";
        else return false;
    }
    return !c.out.empty() && (c.probe || !c.events.empty());
}

// ------------------------------------------------------------------ cell

template <class T>
bool run_cell(const Config& cfg, const std::string& dtype, std::size_t k, std::uint32_t n, std::FILE* csv) {
    const std::size_t result_bytes = std::size_t{n} * sizeof(T);
    const lume::ScalarType scalar = dtype == "f32" ? lume::f32 : lume::i32;
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
    const lume::VerifiedProgram vp = build(scalar, n, k);
    const auto uk = static_cast<std::uint32_t>(k);
    const std::size_t calls = n < 4096 ? 1000 : 1;

    for (const std::string& sname : cfg.strategies) {
        const bool recompute = sname == "R";
        const ExecutionPolicy policy = recompute ? ExecutionPolicy{true, false, true} : ExecutionPolicy{true, false, false};
        // Dry run: the policy must do what its label says, and the result must match the oracle.
        ExecutionStats stats;
        lume_e1::reset_counters();
        lume_e1::arm(nullptr, result_bytes);
        const lume::ExecutionResult dry = lume::detail::execute_cpu_reference_with_policy(vp, inputs, policy, &stats);
        lume_e1::disarm();
        const lume_e1::AllocCounters dc = lume_e1::counters();
        const ExecutionStats expect = recompute ? ExecutionStats{uk, 0, 0, uk, 0, uk, 1} : ExecutionStats{uk, 1, 0, uk, 0, 0, 0};
        if (!dry.ok() || dry.outputs.size() != want.size() || !(stats == expect)) {
            std::fprintf(stderr, "strategy %s did not run as labelled (k=%zu n=%u)\n", sname.c_str(), k, n);
            return false;
        }
        for (std::size_t o = 0; o < want.size(); ++o) {
            if (!matches(dry.outputs[o], want[o])) return false;
        }
        const std::size_t per_call = static_cast<std::size_t>(dc.peak_live_bytes) + (dc.result_allocs + 2) * 2 * lume_e1::Workspace::kHeader;
        lume_e1::Workspace ws((calls * per_call) * 5 / 4 + (1u << 20));
        if (!ws.valid()) return false;

        std::vector<Counter> ctr;
        for (const std::string& en : cfg.events) {
            Counter c;
            c.def = find_event(en);
            if (c.def == nullptr) {
                std::fprintf(stderr, "unknown event %s\n", en.c_str());
                return false;
            }
            c.fd = open_event(*c.def, &c.err);
            ctr.push_back(c);
        }

        std::vector<lume::ExecutionResult> results;
        results.reserve(calls);
        const auto interval = [&] {
            for (std::size_t c = 0; c < calls; ++c) results.push_back(lume::detail::execute_cpu_reference_with_policy(vp, inputs, policy));
        };
        const auto prime = [&] {
            ws.reset();
            lume_e1::arm(&ws, result_bytes);
            interval();
            lume_e1::disarm();
            results.clear();
        };
        for (std::uint32_t w = 0; w < cfg.warmup; ++w) prime();

        for (std::uint32_t rep = 0; rep < cfg.reps; ++rep) {
            prime();
            if (!ws.reset()) return false;
            for (Counter& c : ctr) {
                if (c.fd >= 0) {
                    ioctl(c.fd, PERF_EVENT_IOC_RESET, 0);
                    ioctl(c.fd, PERF_EVENT_IOC_ENABLE, 0);
                }
            }
            lume_e1::reset_counters();
            lume_e1::arm(&ws, result_bytes);
            const auto t0 = Clock::now();
            interval();
            const auto t1 = Clock::now();
            lume_e1::disarm();
            for (Counter& c : ctr) {
                if (c.fd >= 0) ioctl(c.fd, PERF_EVENT_IOC_DISABLE, 0);
            }
            const lume_e1::AllocCounters ac = lume_e1::counters();
            bool ok = results.size() == calls && ac.arena_fallbacks == 0 && ac.table_overflows == 0;
            for (const auto& r : results) {
                ok = ok && r.ok() && r.outputs.size() == want.size();
                for (std::size_t o = 0; ok && o < want.size(); ++o) ok = matches(r.outputs[o], want[o]);
            }
            results.clear();
            if (!ok) {
                std::fprintf(stderr, "CORRECTNESS FAILURE (pmu) %s k=%zu n=%u strategy=%s\n", dtype.c_str(), k, n, sname.c_str());
                return false;
            }
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / static_cast<double>(calls);
            for (Counter& c : ctr) {
                if (c.fd < 0) {
                    std::fprintf(csv, "%s,%s,%s,%zu,%u,%s,%u,%s,0,%d,,,,%.2f,%zu\n", cfg.replicate.c_str(), cfg.compiler.c_str(), dtype.c_str(), k, n,
                                 sname.c_str(), rep, c.def->name, c.err, ns, calls);
                    continue;
                }
                std::uint64_t buf[3] = {0, 0, 0};
                const ssize_t got = read(c.fd, buf, sizeof buf);
                if (got != static_cast<ssize_t>(sizeof buf)) {
                    std::fprintf(csv, "%s,%s,%s,%zu,%u,%s,%u,%s,0,%d,,,,%.2f,%zu\n", cfg.replicate.c_str(), cfg.compiler.c_str(), dtype.c_str(), k, n,
                                 sname.c_str(), rep, c.def->name, errno, ns, calls);
                    continue;
                }
                std::fprintf(csv, "%s,%s,%s,%zu,%u,%s,%u,%s,1,0,%llu,%llu,%llu,%.2f,%zu\n", cfg.replicate.c_str(), cfg.compiler.c_str(), dtype.c_str(), k, n,
                             sname.c_str(), rep, c.def->name, static_cast<unsigned long long>(buf[0]),
                             static_cast<unsigned long long>(buf[1]), static_cast<unsigned long long>(buf[2]), ns, calls);
            }
        }
        for (Counter& c : ctr) {
            if (c.fd >= 0) close(c.fd);
        }
        lume_e1::forget(&ws);
    }
    std::fflush(csv);
    return true;
}

int probe(const Config& cfg) {
    std::FILE* f = std::fopen(cfg.out.c_str(), "w");
    if (f == nullptr) return 2;
    std::fputs("event,supported,errno\n", f);
    for (const EventDef& e : kEvents) {
        int err = 0;
        const int fd = open_event(e, &err);
        std::fprintf(f, "%s,%d,%d\n", e.name, fd >= 0 ? 1 : 0, fd >= 0 ? 0 : err);
        if (fd >= 0) close(fd);
    }
    std::fclose(f);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        std::fprintf(stderr, "usage: %s out=<csv> [probe=1 | compiler=<label> replicate=<id> events=<a,b,..>] [dtypes=..] [ks=..] [sizes=..] [strategies=M,R] [reps=15] [warmup=3] [seed=42]\n", argv[0]);
        return 2;
    }
    if (cfg.probe) return probe(cfg);
    std::FILE* csv = std::fopen(cfg.out.c_str(), "w");
    if (csv == nullptr) return 2;
    std::fputs("replicate,compiler,dtype,fanout,n,strategy,rep,event,supported,errno,value,time_enabled,time_running,ns_per_call,calls\n", csv);
    for (const std::string& dtype : cfg.dtypes) {
        for (const std::uint64_t k : cfg.ks) {
            for (const std::uint64_t n : cfg.sizes) {
                const bool ok = dtype == "f32" ? run_cell<float>(cfg, dtype, k, static_cast<std::uint32_t>(n), csv)
                                               : run_cell<std::int32_t>(cfg, dtype, k, static_cast<std::uint32_t>(n), csv);
                if (!ok) {
                    std::fprintf(stderr, "cell failed: %s k=%llu n=%llu\n", dtype.c_str(), static_cast<unsigned long long>(k), static_cast<unsigned long long>(n));
                    return 1;
                }
                std::fprintf(stderr, "done %s k=%llu n=%llu\n", dtype.c_str(), static_cast<unsigned long long>(k), static_cast<unsigned long long>(n));
            }
        }
    }
    std::fclose(csv);
    return 0;
}

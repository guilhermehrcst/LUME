// Experiment 002 benchmark: footprint and query cost of B1 / B2 / E1 on a
// deterministic event-log workload. See README.md for the pre-registered
// hypothesis, thresholds and method.
//
// Usage: pxir_exp_002 [rows=N] [seed=S] [rounds=R]

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "columnar.hpp"
#include "workload.hpp"

#ifndef PXIR_BUILD_CONFIG
#define PXIR_BUILD_CONFIG "unknown"
#endif
#ifndef PXIR_GIT_SHA
#define PXIR_GIT_SHA "unknown"
#endif

using namespace exp002;
using Clock = std::chrono::steady_clock;

namespace {

// Pre-registered thresholds (README section 2). Do not edit after measuring.
constexpr double kT1MaxBytesRatio = 0.50;  // E1 bytes/row <= 0.50 * B1 bytes/row
constexpr double kT2MaxScanRatio = 1.25;   // max over S1..S3 of median(E1) / median(B1)

struct Options {
    std::size_t rows = 1'000'000;
    u64 seed = 42;
    int rounds = 15;
};

bool parse_u64(const std::string& s, u64& out) {
    if (s.empty() || s[0] == '-') return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = static_cast<u64>(v);
    return true;
}

bool parse_options(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const std::size_t eq = a.find('=');
        if (eq == std::string::npos) return false;
        const std::string key = a.substr(0, eq);
        u64 v = 0;
        if (!parse_u64(a.substr(eq + 1), v)) return false;
        if (key == "rows") {
            o.rows = static_cast<std::size_t>(v);
        } else if (key == "seed") {
            o.seed = v;
        } else if (key == "rounds") {
            if (v == 0 || v > 1000) return false;
            o.rounds = static_cast<int>(v);
        } else {
            return false;
        }
    }
    return o.rows > 0;
}

std::string read_first_match(const char* path, const std::string& key) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key, 0) == 0) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) return line.substr(colon + 1 + (colon + 1 < line.size() && line[colon + 1] == ' ' ? 1 : 0));
        }
    }
    return "n/a";
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double order0_entropy_bits(std::vector<u64> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double n = static_cast<double>(v.size());
    double h = 0.0;
    for (std::size_t i = 0; i < v.size();) {
        std::size_t j = i;
        while (j < v.size() && v[j] == v[i]) ++j;
        const double p = static_cast<double>(j - i) / n;
        h -= p * std::log2(p);
        i = j;
    }
    return h;
}

struct Timed {
    std::array<std::vector<double>, 3> ns_per_unit;  // per layout: one entry per round
};

// Runs the three functions once per round, rotating which goes first, and
// records ns per unit of work. Results are XORed into `sink`.
Timed interleave(int rounds, std::size_t units, const std::array<std::function<u64()>, 3>& fns, u64& sink) {
    Timed t;
    for (const auto& f : fns) sink ^= f();  // warm-up
    for (int r = 0; r < rounds; ++r) {
        for (int k = 0; k < 3; ++k) {
            const std::size_t which = static_cast<std::size_t>((r + k) % 3);
            const auto a = Clock::now();
            sink ^= fns[which]();
            const auto b = Clock::now();
            t.ns_per_unit[which].push_back(std::chrono::duration<double, std::nano>(b - a).count() /
                                           static_cast<double>(units));
        }
    }
    return t;
}

const char* kLayoutName[3] = {"B1", "B2", "E1"};

void print_timed(const char* query, const Timed& t) {
    for (std::size_t l = 0; l < 3; ++l) {
        const auto& v = t.ns_per_unit[l];
        std::printf("time query=%s layout=%s median_ns=%.3f min_ns=%.3f max_ns=%.3f\n", query, kLayoutName[l], median(v),
                    *std::min_element(v.begin(), v.end()), *std::max_element(v.begin(), v.end()));
    }
}

double ratio(const Timed& t, std::size_t num, std::size_t den) { return median(t.ns_per_unit[num]) / median(t.ns_per_unit[den]); }

double ms_since(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_options(argc, argv, opt)) {
        std::fprintf(stderr, "usage: pxir_exp_002 [rows=N>0] [seed=S] [rounds=1..1000]\n");
        return 2;
    }

#if defined(__VERSION__)
    const char* compiler = __VERSION__;
#else
    const char* compiler = "unknown";
#endif
    std::printf("environment cpu=\"%s\" ram_kb=\"%s\" compiler=\"%s\" build_config=%s commit=%s\n",
                read_first_match("/proc/cpuinfo", "model name").c_str(),
                read_first_match("/proc/meminfo", "MemTotal").c_str(), compiler, PXIR_BUILD_CONFIG, PXIR_GIT_SHA);
    std::printf("params rows=%zu seed=%llu rounds=%d\n", opt.rows, static_cast<unsigned long long>(opt.seed), opt.rounds);

    auto t0 = Clock::now();
    const Workload w = generate(opt.rows, opt.seed);
    std::printf("generate ms=%.1f\n", ms_since(t0));
    const std::size_t n = w.size();

    t0 = Clock::now();
    const TypedColumnar b1 = TypedColumnar::build(w);
    std::printf("build layout=B1 ms=%.1f\n", ms_since(t0));
    t0 = Clock::now();
    const DictColumnar b2 = DictColumnar::build(w);
    std::printf("build layout=B2 ms=%.1f\n", ms_since(t0));
    t0 = Clock::now();
    const EncodedColumnar e1 = EncodedColumnar::build(w);
    std::printf("build layout=E1 ms=%.1f\n", ms_since(t0));

    // ---- correctness gate: nothing is timed unless every layout agrees on every row and query
    for (std::size_t i = 0; i < n; ++i) {
        const RowView ref = row_of(w, i);
        if (!same_row(ref, b1.row(i)) || !same_row(ref, b2.row(i)) || !same_row(ref, e1.row(i))) {
            std::fprintf(stderr, "MISMATCH row=%zu\n", i);
            return 1;
        }
    }
    std::vector<std::size_t> name_counts(w.event_name.pool.size(), 0);
    for (const u32 k : w.event_name.idx) ++name_counts[k];
    const std::size_t top = static_cast<std::size_t>(std::max_element(name_counts.begin(), name_counts.end()) - name_counts.begin());
    const std::string target = w.event_name.pool[top];

    u64 ref1 = 0;
    u64 ref2 = 0;
    i64 ref3 = std::numeric_limits<i64>::min();
    for (std::size_t i = 0; i < n; ++i) {
        if (w.event_name.at(i) == target) {
            ++ref1;
            ref2 += static_cast<u64>(w.received[i]) - static_cast<u64>(w.occurred[i]);
        }
        ref3 = std::max(ref3, w.occurred[i]);
    }
    if (b1.s1(target) != ref1 || b2.s1(target) != ref1 || e1.s1(target) != ref1 || b1.s2(target) != ref2 ||
        b2.s2(target) != ref2 || e1.s2(target) != ref2 || b1.s3() != ref3 || b2.s3() != ref3 || e1.s3() != ref3) {
        std::fprintf(stderr, "MISMATCH query\n");
        return 1;
    }

    // ---- footprint (exact byte accounting)
    const Breakdown bd[3] = {b1.breakdown(), b2.breakdown(), e1.breakdown()};
    const double rows_d = static_cast<double>(n);
    double bytes_per_row[3];
    for (std::size_t l = 0; l < 3; ++l) {
        bytes_per_row[l] = static_cast<double>(bd[l].total()) / rows_d;
        std::printf("footprint layout=%s bytes=%zu bytes_per_row=%.3f\n", kLayoutName[l], bd[l].total(), bytes_per_row[l]);
        for (const auto& p : bd[l].parts) {
            std::printf("footprint_part layout=%s column=%s bytes_per_row=%.3f\n", kLayoutName[l], p.first.c_str(),
                        static_cast<double>(p.second) / rows_d);
        }
    }
    const double e1_no_id = static_cast<double>(bd[2].total() - bd[2].bytes_of("event_id")) / rows_d;
    const double b1_no_id = static_cast<double>(bd[0].total() - bd[0].bytes_of("event_id")) / rows_d;
    const double b2_no_id = static_cast<double>(bd[1].total() - bd[1].bytes_of("event_id")) / rows_d;
    std::printf("footprint_without_event_id layout=B1 bytes_per_row=%.3f\n", b1_no_id);
    std::printf("footprint_without_event_id layout=B2 bytes_per_row=%.3f\n", b2_no_id);
    std::printf("footprint_without_event_id layout=E1 bytes_per_row=%.3f\n", e1_no_id);

    // ---- order-0 entropy vs bits actually used by E1 (informational)
    {
        const auto entropy_col = [&](const char* name, const StringColumn& c) {
            const double h = order0_entropy_bits(widen(c.idx));
            std::printf("entropy column=%s order0_bits_per_row=%.3f e1_bits_per_row=%.3f\n", name, h,
                        static_cast<double>(bd[2].bytes_of(name)) * 8.0 / rows_d);
        };
        entropy_col("event_name", w.event_name);
        entropy_col("route", w.route);
        entropy_col("properties", w.properties);
        entropy_col("acquisition", w.acquisition);

        std::vector<u64> gaps(n);
        std::vector<u64> lat(n);
        for (std::size_t i = 0; i < n; ++i) {
            const bool first = i == 0 || w.session_id[i] != w.session_id[i - 1];
            const u64 expected = first ? 0 : static_cast<u64>(w.client_seq[i - 1]) + 1;
            gaps[i] = zigzag(static_cast<i64>(static_cast<u64>(w.client_seq[i]) - expected));
            lat[i] = zigzag(static_cast<i64>(static_cast<u64>(w.received[i]) - static_cast<u64>(w.occurred[i])));
        }
        std::printf("entropy column=client_seq order0_bits_per_row=%.3f e1_bits_per_row=%.3f\n", order0_entropy_bits(gaps),
                    static_cast<double>(bd[2].bytes_of("client_seq")) * 8.0 / rows_d);
        std::printf("entropy column=received_at_minus_occurred_at order0_bits_per_row=%.3f e1_bits_per_row=%.3f\n",
                    order0_entropy_bits(lat), static_cast<double>(bd[2].bytes_of("received_at")) * 8.0 / rows_d);
    }

    // ---- query timing, interleaved A/B/C
    u64 sink = 0;
    const std::array<std::function<u64()>, 3> s1 = {[&] { return b1.s1(target); }, [&] { return b2.s1(target); },
                                                    [&] { return e1.s1(target); }};
    const std::array<std::function<u64()>, 3> s2 = {[&] { return b1.s2(target); }, [&] { return b2.s2(target); },
                                                    [&] { return e1.s2(target); }};
    const std::array<std::function<u64()>, 3> s3 = {[&] { return static_cast<u64>(b1.s3()); },
                                                    [&] { return static_cast<u64>(b2.s3()); },
                                                    [&] { return static_cast<u64>(e1.s3()); }};
    const Timed t1 = interleave(opt.rounds, n, s1, sink);
    const Timed t2 = interleave(opt.rounds, n, s2, sink);
    const Timed t3 = interleave(opt.rounds, n, s3, sink);
    print_timed("S1", t1);
    print_timed("S2", t2);
    print_timed("S3", t3);

    // ---- point lookups (informational)
    const std::size_t lookups = std::min<std::size_t>(200000, n);
    std::vector<std::size_t> picks(lookups);
    {
        Rng pick_rng(opt.seed ^ 0xABCDEF1234567ull);
        for (std::size_t& p : picks) p = static_cast<std::size_t>(pick_rng.next() % n);
    }
    const auto lookup_sum = [&](const auto& layout) {
        u64 acc = 0;
        for (const std::size_t p : picks) acc += checksum(layout.row(p));
        return acc;
    };
    u64 ref_p1 = 0;
    for (const std::size_t p : picks) ref_p1 += checksum(row_of(w, p));
    const std::array<std::function<u64()>, 3> p1 = {[&] { return lookup_sum(b1); }, [&] { return lookup_sum(b2); },
                                                    [&] { return lookup_sum(e1); }};
    for (const auto& f : p1) {
        if (f() != ref_p1) {
            std::fprintf(stderr, "MISMATCH lookup\n");
            return 1;
        }
    }
    const Timed tp = interleave(opt.rounds, lookups, p1, sink);
    print_timed("P1", tp);

    // ---- verdicts against the pre-registered thresholds
    const double bytes_ratio = bytes_per_row[2] / bytes_per_row[0];
    const double r1 = ratio(t1, 2, 0);
    const double r2 = ratio(t2, 2, 0);
    const double r3 = ratio(t3, 2, 0);
    const double worst = std::max({r1, r2, r3});
    std::printf("ratio_vs_B1 bytes=%.4f S1=%.3f S2=%.3f S3=%.3f P1=%.3f\n", bytes_ratio, r1, r2, r3, ratio(tp, 2, 0));
    std::printf("ratio_vs_B2 bytes=%.4f S1=%.3f S2=%.3f S3=%.3f P1=%.3f\n", bytes_per_row[2] / bytes_per_row[1],
                ratio(t1, 2, 1), ratio(t2, 2, 1), ratio(t3, 2, 1), ratio(tp, 2, 1));
    std::printf("verdict T1 threshold=%.2f observed=%.4f %s\n", kT1MaxBytesRatio, bytes_ratio,
                bytes_ratio <= kT1MaxBytesRatio ? "PASS" : "FAIL");
    std::printf("verdict T2 threshold=%.2f observed_worst=%.3f %s\n", kT2MaxScanRatio, worst,
                worst <= kT2MaxScanRatio ? "PASS" : "FAIL");
    std::printf("sink=%llu\n", static_cast<unsigned long long>(sink));
    std::printf("correctness=exact\n");
    return 0;
}

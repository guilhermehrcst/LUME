#pragma once

// Logical model and deterministic generator for experiment 002.
//
// The workload is synthetic. It is calibrated to aggregate statistics only
// (see README section 3). Every layout under test must reproduce the logical
// rows defined here bit for bit.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace exp002 {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;
using Id128 = std::array<u64, 2>;

// A text column stored as a pool of distinct strings plus one pool index per row.
// This is the generator's source of truth, not one of the layouts under test.
struct StringColumn {
    std::vector<std::string> pool;
    std::vector<u32> idx;

    [[nodiscard]] std::string_view at(std::size_t i) const noexcept { return pool[idx[i]]; }
};

struct Workload {
    std::vector<Id128> event_id;
    std::vector<Id128> session_id;
    std::vector<i64> client_seq;
    std::vector<u32> schema_version;
    std::vector<i64> occurred;
    std::vector<i64> received;
    StringColumn mode;
    StringColumn event_name;
    StringColumn route;
    StringColumn acquisition;
    StringColumn properties;

    [[nodiscard]] std::size_t size() const noexcept { return event_id.size(); }
};

// One fully materialized logical row. Text fields are views into layout storage.
struct RowView {
    Id128 event_id{};
    Id128 session_id{};
    i64 seq = 0;
    u32 schema_version = 0;
    i64 occurred = 0;
    i64 received = 0;
    std::string_view mode;
    std::string_view event_name;
    std::string_view route;
    std::string_view acquisition;
    std::string_view properties;
};

[[nodiscard]] inline bool same_row(const RowView& a, const RowView& b) noexcept {
    return a.event_id == b.event_id && a.session_id == b.session_id && a.seq == b.seq &&
           a.schema_version == b.schema_version && a.occurred == b.occurred && a.received == b.received &&
           a.mode == b.mode && a.event_name == b.event_name && a.route == b.route &&
           a.acquisition == b.acquisition && a.properties == b.properties;
}

[[nodiscard]] inline RowView row_of(const Workload& w, std::size_t i) noexcept {
    return RowView{w.event_id[i],   w.session_id[i], w.client_seq[i],     w.schema_version[i],
                   w.occurred[i],   w.received[i],   w.mode.at(i),        w.event_name.at(i),
                   w.route.at(i),   w.acquisition.at(i), w.properties.at(i)};
}

[[nodiscard]] inline u64 mix(u64 h, u64 v) noexcept {
    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

[[nodiscard]] inline u64 mix_text(u64 h, std::string_view s) noexcept {
    h = mix(h, static_cast<u64>(s.size()));
    for (const char c : s) h = (h ^ static_cast<u64>(static_cast<unsigned char>(c))) * 0x100000001b3ull;
    return h;
}

// Cheap content checksum of one row; used by the point-lookup benchmark so the
// work cannot be optimized away and layouts can be compared.
[[nodiscard]] inline u64 checksum(const RowView& r) noexcept {
    u64 h = 0xcbf29ce484222325ull;
    h = mix(h, r.event_id[0]);
    h = mix(h, r.event_id[1]);
    h = mix(h, r.session_id[0]);
    h = mix(h, r.session_id[1]);
    h = mix(h, static_cast<u64>(r.seq));
    h = mix(h, static_cast<u64>(r.schema_version));
    h = mix(h, static_cast<u64>(r.occurred));
    h = mix(h, static_cast<u64>(r.received));
    h = mix_text(h, r.mode);
    h = mix_text(h, r.event_name);
    h = mix_text(h, r.route);
    h = mix_text(h, r.acquisition);
    h = mix_text(h, r.properties);
    return h;
}

// std::mt19937_64's output sequence is fixed by the standard; the distributions
// below are hand-written so the workload does not depend on the standard library.
class Rng {
public:
    explicit Rng(u64 seed) : engine_(seed) {}

    u64 next() { return engine_(); }
    double uniform() { return static_cast<double>(engine_() >> 11) * 0x1p-53; }
    double normal() {
        const double u1 = 1.0 - uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }
    double lognormal(double mu, double sigma) { return std::exp(mu + sigma * normal()); }

private:
    std::mt19937_64 engine_;
};

// Zipf-shaped probabilities over `card` values whose entropy is `target_bits`.
[[nodiscard]] inline std::vector<double> zipf_probabilities(std::size_t card, double target_bits) {
    if (card <= 1) return std::vector<double>(card, 1.0);
    const auto probs = [card](double s) {
        std::vector<double> p(card);
        double sum = 0.0;
        for (std::size_t k = 0; k < card; ++k) {
            p[k] = std::pow(static_cast<double>(k + 1), -s);
            sum += p[k];
        }
        for (double& x : p) x /= sum;
        return p;
    };
    const auto entropy = [](const std::vector<double>& p) {
        double h = 0.0;
        for (const double x : p) {
            if (x > 0.0) h -= x * std::log2(x);
        }
        return h;
    };
    if (target_bits >= std::log2(static_cast<double>(card))) return probs(0.0);
    double lo = 0.0;
    double hi = 60.0;
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (entropy(probs(mid)) > target_bits) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return probs(0.5 * (lo + hi));
}

class Discrete {
public:
    explicit Discrete(const std::vector<double>& probabilities) {
        double acc = 0.0;
        for (const double p : probabilities) {
            acc += p;
            cdf_.push_back(acc);
        }
    }

    u32 sample(Rng& rng) const {
        const double u = rng.uniform();
        const auto it = std::upper_bound(cdf_.begin(), cdf_.end(), u);
        const std::size_t k = static_cast<std::size_t>(it - cdf_.begin());
        return static_cast<u32>(std::min(k, cdf_.size() - 1));
    }

private:
    std::vector<double> cdf_;
};

[[nodiscard]] inline std::string random_text(Rng& rng, std::size_t len) {
    std::string s(len, 'a');
    for (char& c : s) c = static_cast<char>('a' + static_cast<int>(rng.next() % 26));
    return s;
}

[[nodiscard]] inline std::vector<std::string> random_pool(Rng& rng, std::size_t card, std::size_t min_len,
                                                          std::size_t max_len) {
    std::vector<std::string> pool;
    pool.reserve(card);
    for (std::size_t k = 0; k < card; ++k) {
        const std::size_t len = min_len + static_cast<std::size_t>(rng.next() % (max_len - min_len + 1));
        pool.push_back(random_text(rng, len));
    }
    return pool;
}

inline constexpr i64 kBaseEpochMs = 1'760'000'000'000;

// Generates `rows` events grouped into sessions, deterministically from `seed`.
[[nodiscard]] inline Workload generate(std::size_t rows, u64 seed) {
    Rng rng(seed);
    Workload w;

    w.mode.pool = {"live"};
    w.event_name.pool = random_pool(rng, 14, 10, 10);
    w.route.pool = random_pool(rng, 51, 10, 10);
    w.properties.pool = random_pool(rng, 102, 10, 31);
    w.acquisition.pool = {"{}", "{\"a\":1}"};

    const Discrete name_dist(zipf_probabilities(14, 2.157));
    const Discrete route_dist(zipf_probabilities(51, 3.403));
    const Discrete props_dist(zipf_probabilities(102, 4.022));
    const Discrete acq_dist(zipf_probabilities(2, 0.004));

    w.event_id.reserve(rows);
    w.session_id.reserve(rows);
    w.client_seq.reserve(rows);
    w.schema_version.reserve(rows);
    w.occurred.reserve(rows);
    w.received.reserve(rows);

    while (w.size() < rows) {
        const double raw_len = std::min(std::floor(rng.lognormal(1.7, 1.5)), 199.0);
        const std::size_t len = std::min(static_cast<std::size_t>(raw_len) + 1, rows - w.size());
        const Id128 sid{rng.next(), rng.next()};
        const i64 start = kBaseEpochMs + static_cast<i64>(rng.uniform() * 30.0 * 86400.0 * 1000.0);
        const i64 skew = rng.uniform() < 0.005 ? static_cast<i64>(60000.0 + rng.uniform() * 130000.0) : 0;

        i64 t = start;
        i64 seq = 0;
        for (std::size_t j = 0; j < len; ++j) {
            if (j > 0) {
                t += static_cast<i64>(std::min(rng.lognormal(-0.82, 3.29), 58600.0) * 1000.0);
                seq += 1;
                if (rng.uniform() < 0.003) seq += 1 + static_cast<i64>(rng.next() % 14);
            }
            const i64 latency = static_cast<i64>(std::min(rng.lognormal(4.663, 1.907), 2751.0));
            w.event_id.push_back(Id128{rng.next(), rng.next()});
            w.session_id.push_back(sid);
            w.client_seq.push_back(seq);
            w.schema_version.push_back(1);
            w.occurred.push_back(t + skew);
            w.received.push_back(t + latency);
            w.mode.idx.push_back(0);
            w.event_name.idx.push_back(name_dist.sample(rng));
            w.route.idx.push_back(route_dist.sample(rng));
            w.properties.idx.push_back(props_dist.sample(rng));
            w.acquisition.idx.push_back(acq_dist.sample(rng));
        }
    }
    return w;
}

}  // namespace exp002

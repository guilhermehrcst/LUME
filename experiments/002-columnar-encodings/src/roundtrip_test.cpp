// Correctness gate for experiment 002: every layout must reproduce every
// logical row bit for bit and answer every query identically, including on
// adversarial inputs. Prints "exp002_roundtrip=PASS" only if nothing failed.

#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <vector>

#include "columnar.hpp"
#include "workload.hpp"

using namespace exp002;

namespace {

int g_failures = 0;

void fail(const std::string& what) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

// ---- packing primitives, every width 0..64 and block-boundary sizes

void test_packing() {
    Rng rng(7);
    const std::size_t sizes[] = {0, 1, 2, 127, 128, 129, 300};
    for (unsigned w = 0; w <= 64; ++w) {
        for (const std::size_t n : sizes) {
            std::vector<u64> values(n);
            for (std::size_t i = 0; i < n; ++i) {
                u64 v = rng.next();
                if (w < 64) v &= (u64{1} << w) - 1;
                if (i % 5 == 0 && w > 0) v = (w == 64) ? ~u64{0} : ((u64{1} << w) - 1);  // all ones
                values[i] = v;
            }
            const FixedPacked f = FixedPacked::pack(values, w);
            for (std::size_t i = 0; i < n; ++i) {
                if (f.get(i) != values[i]) {
                    fail("FixedPacked.get width=" + std::to_string(w) + " n=" + std::to_string(n));
                    break;
                }
            }
            const BlockPacked b = BlockPacked::pack(values);
            for (std::size_t i = 0; i < n; ++i) {
                if (b.get(i) != values[i]) {
                    fail("BlockPacked.get width=" + std::to_string(w) + " n=" + std::to_string(n));
                    break;
                }
            }
            std::size_t seen = 0;
            bool ok = true;
            b.for_each_block([&](std::size_t first, const u64* v, std::size_t count) {
                if (first != seen) ok = false;
                for (std::size_t k = 0; k < count; ++k) {
                    if (v[k] != values[first + k]) ok = false;
                }
                seen += count;
            });
            if (!ok || seen != n) fail("BlockPacked.for_each_block width=" + std::to_string(w) + " n=" + std::to_string(n));
        }
    }
    // fail closed: a value that does not fit its width must be rejected, not truncated
    try {
        const std::vector<u64> too_big{4};
        (void)FixedPacked::pack(too_big, 2);
        fail("FixedPacked accepted a value wider than its width");
    } catch (const std::invalid_argument&) {
    }
    for (const i64 x : {std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max(), i64{0}, i64{-1}, i64{1}, i64{-12345}}) {
        if (unzigzag(zigzag(x)) != x) fail("zigzag round trip");
    }
}

// ---- layouts

u64 ref_s1(const Workload& w, std::string_view t) {
    u64 c = 0;
    for (std::size_t i = 0; i < w.size(); ++i) c += static_cast<u64>(w.event_name.at(i) == t);
    return c;
}
u64 ref_s2(const Workload& w, std::string_view t) {
    u64 a = 0;
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (w.event_name.at(i) == t) a += static_cast<u64>(w.received[i]) - static_cast<u64>(w.occurred[i]);
    }
    return a;
}
i64 ref_s3(const Workload& w) {
    i64 b = std::numeric_limits<i64>::min();
    for (const i64 t : w.occurred) b = std::max(b, t);
    return b;
}

template <class Layout>
void check_layout(const char* layout, const Layout& l, const Workload& w, const std::string& label,
                  const std::string& target) {
    if (l.size() != w.size()) {
        fail(std::string(layout) + " size mismatch: " + label);
        return;
    }
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (!same_row(row_of(w, i), l.row(i))) {
            fail(std::string(layout) + " row " + std::to_string(i) + " differs: " + label);
            return;
        }
    }
    if (l.s1(target) != ref_s1(w, target)) fail(std::string(layout) + " S1 differs: " + label);
    if (l.s2(target) != ref_s2(w, target)) fail(std::string(layout) + " S2 differs: " + label);
    if (l.s3() != ref_s3(w)) fail(std::string(layout) + " S3 differs: " + label);
    if (l.s1("no-such-event-name") != 0 || l.s2("no-such-event-name") != 0) fail(std::string(layout) + " absent target: " + label);
}

void check_workload(const Workload& w, const std::string& label) {
    const std::string target = w.size() == 0 ? std::string("x") : std::string(w.event_name.at(0));
    check_layout("B1", TypedColumnar::build(w), w, label, target);
    check_layout("B2", DictColumnar::build(w), w, label, target);
    check_layout("E1", EncodedColumnar::build(w), w, label, target);
}

// ---- adversarial workloads

Workload make_adversarial(int kind, std::size_t n, u64 seed) {
    Rng rng(seed);
    Workload w;
    const std::vector<std::string> pool{"", std::string("a\0b", 3), std::string(300, 'z'), "\xc3\xbc" "n\xc3\xaf", "plain", "plain "};
    for (StringColumn* c : {&w.mode, &w.event_name, &w.route, &w.acquisition, &w.properties}) c->pool = pool;

    const i64 specials[] = {std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max(), 0, -1, 1};
    const auto wild = [&]() -> i64 {
        const u64 pick = rng.next() % 8;
        if (pick < 5) return specials[pick];
        return static_cast<i64>(rng.next());
    };

    const Id128 few[] = {{1, 2}, {3, 4}, {5, 6}, {0, 0}};
    i64 mono = 0;
    for (std::size_t i = 0; i < n; ++i) {
        Id128 sid{};
        i64 seq = 0;
        i64 occ = 0;
        i64 rec = 0;
        switch (kind) {
            case 0:  // extreme values, sessions interleaved (not clustered)
                sid = few[rng.next() % 4];
                seq = wild();
                occ = wild();
                rec = wild();
                break;
            case 1:  // one session; monotone seq; timestamps jump by huge steps
                sid = few[0];
                seq = static_cast<i64>(i);
                mono = static_cast<i64>(static_cast<u64>(mono) + (rng.next() >> 2));  // wraps by design
                occ = mono;
                rec = static_cast<i64>(static_cast<u64>(occ) + rng.next() % 3000 - 1500);
                break;
            case 2:  // a distinct session per row
                sid = Id128{i + 1, ~static_cast<u64>(i)};
                seq = static_cast<i64>(rng.next());
                occ = wild();
                rec = wild();
                break;
            default:  // identical rows except the id
                sid = few[1];
                seq = 7;
                occ = 42;
                rec = 42;
                break;
        }
        w.event_id.push_back(Id128{rng.next(), rng.next()});
        w.session_id.push_back(sid);
        w.client_seq.push_back(seq);
        w.schema_version.push_back(kind == 0 ? static_cast<u32>(rng.next()) : 9U);
        w.occurred.push_back(occ);
        w.received.push_back(rec);
        const u32 pn = static_cast<u32>(pool.size());
        const bool same = kind == 3;
        w.mode.idx.push_back(same ? 0 : static_cast<u32>(rng.next() % pn));
        w.event_name.idx.push_back(same ? 1 : static_cast<u32>(rng.next() % pn));
        w.route.idx.push_back(same ? 2 : static_cast<u32>(rng.next() % pn));
        w.acquisition.idx.push_back(same ? 3 : static_cast<u32>(rng.next() % pn));
        w.properties.idx.push_back(same ? 4 : static_cast<u32>(rng.next() % pn));
    }
    return w;
}

}  // namespace

int main() {
    try {
        test_packing();

        const std::size_t sizes[] = {0, 1, 2, 127, 128, 129, 1000, 50000};
        for (const u64 seed : {u64{1}, u64{2}, u64{3}}) {
            for (const std::size_t n : sizes) {
                check_workload(generate(n, seed), "generated n=" + std::to_string(n) + " seed=" + std::to_string(seed));
            }
        }
        const std::size_t adv_sizes[] = {0, 1, 2, 127, 128, 129, 1000, 20000};
        for (int kind = 0; kind < 4; ++kind) {
            for (const std::size_t n : adv_sizes) {
                check_workload(make_adversarial(kind, n, static_cast<u64>(100 + kind)),
                               "adversarial kind=" + std::to_string(kind) + " n=" + std::to_string(n));
            }
        }
    } catch (const std::exception& e) {
        fail(std::string("unexpected exception: ") + e.what());
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "exp002_roundtrip=FAIL failures=%d\n", g_failures);
        return 1;
    }
    std::printf("exp002_roundtrip=PASS\n");
    return 0;
}

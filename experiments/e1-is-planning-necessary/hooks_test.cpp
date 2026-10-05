// E1 allocation hooks: result-sized requests are counted and routed to the
// workspace while armed; small requests and disarmed requests are not; blocks
// return to the right owner; exhaustion falls back to malloc and is counted.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "alloc_hooks.hpp"
#include "check.hpp"

using namespace lume_e1;

namespace {

bool aligned(const void* p) { return reinterpret_cast<std::uintptr_t>(p) % Workspace::kAlign == 0; }

void routing_and_counting() {
    Workspace ws(1 << 20);
    reset_counters();
    arm(&ws, 4000);
    {
        auto big = std::make_unique<float[]>(1000);      // 4000 B: result-sized
        auto other = std::make_unique<float[]>(1001);    // 4004 B: a different size is ignored
        std::vector<float> v(1000);                      // 4000 B through std::allocator: result-sized
        auto small = std::make_unique<float[]>(8);       // 32 B: ignored
        LUME_CHECK(ws.owns(big.get()) && aligned(big.get()));
        LUME_CHECK(!ws.owns(other.get()) && !ws.owns(small.get()));
        LUME_CHECK(ws.owns(v.data()));
        LUME_CHECK(counters().result_allocs == 2);
        LUME_CHECK(counters().arena_allocs == 2);
        LUME_CHECK(counters().result_alloc_bytes == 8000);
        LUME_CHECK(counters().live_bytes == 8000 && counters().peak_live_bytes == 8000);
        LUME_CHECK(ws.live_blocks() == 2);
    }
    LUME_CHECK(ws.live_blocks() == 0);
    LUME_CHECK(counters().live_bytes == 0 && counters().peak_live_bytes == 8000);
    LUME_CHECK(counters().result_frees == 2);
    disarm();
    forget(&ws);
}

void disarmed_and_no_workspace() {
    Workspace ws(1 << 20);
    reset_counters();
    disarm();
    {
        auto p = std::make_unique<float[]>(5000);
        LUME_CHECK(!ws.owns(p.get()));
        LUME_CHECK(counters().result_allocs == 0);
    }
    arm(nullptr, 20000);  // counting only, malloc serves
    {
        auto p = std::make_unique<float[]>(5000);
        LUME_CHECK(!ws.owns(p.get()));
        LUME_CHECK(counters().result_allocs == 1 && counters().arena_allocs == 0);
        LUME_CHECK(counters().live_bytes == 20000 && counters().table_overflows == 0);
    }
    LUME_CHECK(counters().live_bytes == 0 && counters().result_frees == 1);
    disarm();
}

void block_survives_disarm_and_is_released_to_owner() {
    Workspace ws(1 << 20);
    reset_counters();
    arm(&ws, 20000);
    auto p = std::make_unique<float[]>(5000);
    LUME_CHECK(ws.owns(p.get()));
    disarm();      // routing off; the block must still go back to the workspace
    arm(nullptr, 20000);
    p.reset();     // would be free() on an arena pointer if ownership were forgotten
    LUME_CHECK(ws.live_blocks() == 0);
    disarm();
    forget(&ws);
}

void exhaustion_falls_back_and_is_counted() {
    Workspace ws(64 * 1024);
    reset_counters();
    arm(&ws, 16 * 1024);
    std::vector<std::unique_ptr<char[]>> blocks;
    for (int i = 0; i < 6; ++i) blocks.push_back(std::make_unique<char[]>(16 * 1024));
    const AllocCounters c = counters();
    LUME_CHECK(c.result_allocs == 6);
    LUME_CHECK(c.arena_allocs + c.arena_fallbacks == 6);
    LUME_CHECK(c.arena_fallbacks >= 1 && c.arena_allocs >= 1);
    blocks.clear();
    LUME_CHECK(ws.live_blocks() == 0);
    LUME_CHECK(counters().live_bytes == 0);
    disarm();
    forget(&ws);
}

// Many counted malloc blocks live at once (an interval holds K = 1000 results),
// freed in allocation order, in reverse and in a scrambled order, with small
// uncounted allocations interleaved: accounting must stay exact.
void many_live_blocks_exact_accounting() {
    for (const int order : {0, 1, 2}) {
        reset_counters();
        arm(nullptr, 4000);
        std::vector<std::unique_ptr<float[]>> blocks;
        std::vector<std::unique_ptr<char[]>> small;
        for (int i = 0; i < 3000; ++i) {
            blocks.push_back(std::make_unique<float[]>(1000));  // 4000 B counted
            small.push_back(std::make_unique<char[]>(24));      // uncounted
        }
        LUME_CHECK(counters().result_allocs == 3000 && counters().table_overflows == 0);
        LUME_CHECK(counters().live_bytes == 3000 * 4000 && counters().peak_live_bytes == 3000 * 4000);
        std::vector<std::size_t> idx(blocks.size());
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = order == 1 ? idx.size() - 1 - i : i;
        if (order == 2) {
            std::uint64_t x = 88172645463325252ull;  // xorshift: deterministic scramble
            for (std::size_t i = idx.size() - 1; i > 0; --i) {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                std::swap(idx[i], idx[static_cast<std::size_t>(x % (i + 1))]);
            }
        }
        for (const std::size_t i : idx) {
            blocks[i].reset();
            if (i % 3 == 0) small[i].reset();
        }
        LUME_CHECK(counters().live_bytes == 0 && counters().result_frees == 3000);
        small.clear();
        disarm();
    }
    // More live counted blocks than the table holds are reported, never mis-counted silently.
    reset_counters();
    arm(nullptr, 4000);
    {
        std::vector<std::unique_ptr<float[]>> blocks;
        for (int i = 0; i < 5000; ++i) blocks.push_back(std::make_unique<float[]>(1000));
        LUME_CHECK(counters().table_overflows > 0);
    }
    disarm();
}

}  // namespace

int main() {
    many_live_blocks_exact_accounting();
    routing_and_counting();
    disarmed_and_no_workspace();
    block_survives_disarm_and_is_released_to_owner();
    exhaustion_falls_back_and_is_counted();
    return lume_test::finish("e1_hooks");
}

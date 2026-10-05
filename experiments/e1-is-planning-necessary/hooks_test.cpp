// E1 allocation hooks: result-sized requests are counted and routed to the
// workspace while armed; small requests and disarmed requests are not; blocks
// return to the right owner; exhaustion falls back to malloc and is counted.

#include <cstdint>
#include <memory>
#include <vector>

#include "alloc_hooks.hpp"
#include "check.hpp"

using namespace lume_e1;

namespace {

bool aligned(const void* p) { return reinterpret_cast<std::uintptr_t>(p) % Workspace::kAlign == 0; }

void routing_and_counting() {
    Workspace ws(1 << 20);
    reset_counters();
    arm(&ws, 1024);
    {
        auto big = std::make_unique<float[]>(1000);      // 4000 B: result-sized
        auto small = std::make_unique<float[]>(8);       // 32 B: ignored
        std::vector<float> v(2000);                      // 8000 B through std::allocator
        LUME_CHECK(ws.owns(big.get()) && aligned(big.get()));
        LUME_CHECK(!ws.owns(small.get()));
        LUME_CHECK(ws.owns(v.data()));
        LUME_CHECK(counters().result_allocs == 2);
        LUME_CHECK(counters().arena_allocs == 2);
        LUME_CHECK(counters().result_alloc_bytes == 4000 + 8000);
        LUME_CHECK(counters().live_bytes == 12000 && counters().peak_live_bytes == 12000);
        LUME_CHECK(ws.live_blocks() == 2);
    }
    LUME_CHECK(ws.live_blocks() == 0);
    LUME_CHECK(counters().live_bytes == 0 && counters().peak_live_bytes == 12000);
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
    arm(nullptr, 1024);  // counting only, malloc serves
    {
        auto p = std::make_unique<float[]>(5000);
        LUME_CHECK(!ws.owns(p.get()));
        LUME_CHECK(counters().result_allocs == 1 && counters().arena_allocs == 0);
        LUME_CHECK(counters().live_bytes >= 20000);
    }
    LUME_CHECK(counters().live_bytes == 0);
    disarm();
}

void block_survives_disarm_and_is_released_to_owner() {
    Workspace ws(1 << 20);
    reset_counters();
    arm(&ws, 1024);
    auto p = std::make_unique<float[]>(5000);
    LUME_CHECK(ws.owns(p.get()));
    disarm();      // routing off; the block must still go back to the workspace
    arm(nullptr, 1024);
    p.reset();     // would be free() on an arena pointer if ownership were forgotten
    LUME_CHECK(ws.live_blocks() == 0);
    disarm();
    forget(&ws);
}

void exhaustion_falls_back_and_is_counted() {
    Workspace ws(64 * 1024);
    reset_counters();
    arm(&ws, 1024);
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

}  // namespace

int main() {
    routing_and_counting();
    disarmed_and_no_workspace();
    block_survives_disarm_and_is_released_to_owner();
    exhaustion_falls_back_and_is_counted();
    return lume_test::finish("e1_hooks");
}

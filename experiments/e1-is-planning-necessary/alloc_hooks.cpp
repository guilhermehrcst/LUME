#include "alloc_hooks.hpp"

#include <cstdint>
#include <cstdlib>
#include <new>

namespace lume_e1 {
namespace {

// Live counted malloc blocks, by pointer, in an open-addressing hash set with
// backward-shift deletion (no tombstones), so insert, find and erase are O(1)
// however many results an interval holds. (A first version scanned a flat
// array on every free, which made each free of an uncounted pointer cost
// O(live blocks) and inflated small-N timings by about 1 us per call whenever
// K = 1000 results were held. That run is kept as superseded.)
struct Slot {
    void* p;
    std::size_t n;
};
constexpr std::size_t kTableBits = 15;                 // 32768 slots (E2: k = 8 results x K = 1000 held; E1 used 13)
constexpr std::size_t kTable = std::size_t{1} << kTableBits;
constexpr std::size_t kMaxLive = kTable / 2;           // keep the load factor <= 1/2

struct State {
    bool armed = false;
    Workspace* ws = nullptr;     // routing target for result-sized requests
    Workspace* owner = nullptr;  // most recent workspace: ownership checks on delete
    std::size_t bytes = 0;       // exact result size
    AllocCounters c;
    Slot slots[kTable];
    std::size_t live_count = 0;
};

State& st() noexcept {
    static State s;  // constant-initialized: safe before main
    return s;
}

std::size_t home(const void* p) noexcept {
    return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> (64 - kTableBits));
}

bool table_insert(State& s, void* p, std::size_t n) noexcept {
    if (s.live_count >= kMaxLive) return false;
    std::size_t i = home(p);
    while (s.slots[i].p != nullptr) i = (i + 1) & (kTable - 1);
    s.slots[i] = {p, n};
    ++s.live_count;
    return true;
}

// Removes p and returns its recorded size; false when p is not a counted block.
bool table_erase(State& s, void* p, std::size_t& n) noexcept {
    if (s.live_count == 0) return false;
    std::size_t i = home(p);
    while (s.slots[i].p != nullptr && s.slots[i].p != p) i = (i + 1) & (kTable - 1);
    if (s.slots[i].p == nullptr) return false;
    n = s.slots[i].n;
    // Backward-shift: pull later members of the probe run into the hole.
    std::size_t hole = i;
    std::size_t j = i;
    for (;;) {
        j = (j + 1) & (kTable - 1);
        if (s.slots[j].p == nullptr) break;
        const std::size_t h = home(s.slots[j].p);
        // Move slot j into the hole unless its home lies cyclically in (hole, j].
        const bool stays = hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
        if (!stays) {
            s.slots[hole] = s.slots[j];
            hole = j;
        }
    }
    s.slots[hole] = {nullptr, 0};
    --s.live_count;
    return true;
}

void note_live(State& s, std::size_t n) noexcept {
    s.c.live_bytes += static_cast<std::int64_t>(n);
    if (s.c.live_bytes > s.c.peak_live_bytes) s.c.peak_live_bytes = s.c.live_bytes;
}

void* allocate(std::size_t n) {
    State& s = st();
    if (s.armed && n == s.bytes) {
        ++s.c.result_allocs;
        s.c.result_alloc_bytes += n;
        if (s.ws != nullptr) {
            if (void* p = s.ws->allocate(n)) {
                ++s.c.arena_allocs;
                note_live(s, n);
                return p;
            }
            ++s.c.arena_fallbacks;
        }
        void* p = std::malloc(n);
        if (p == nullptr) throw std::bad_alloc();
        if (table_insert(s, p, n)) {
            note_live(s, n);
        } else {
            ++s.c.table_overflows;
        }
        return p;
    }
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

void release(void* p) noexcept {
    if (p == nullptr) return;
    State& s = st();
    if (s.owner != nullptr && s.owner->owns(p)) {
        if (s.armed) {
            s.c.live_bytes -= static_cast<std::int64_t>(s.owner->requested_size(p));
            ++s.c.result_frees;
        }
        s.owner->deallocate(p);
        return;
    }
    std::size_t counted = 0;
    if (table_erase(s, p, counted) && s.armed) {
        s.c.live_bytes -= static_cast<std::int64_t>(counted);
        ++s.c.result_frees;
    }
    std::free(p);
}

}  // namespace

void arm(Workspace* ws, std::size_t bytes) noexcept {
    State& s = st();
    s.ws = ws;
    if (ws != nullptr) s.owner = ws;
    s.bytes = bytes;
    s.armed = true;
}
void disarm() noexcept { st().armed = false; }
void forget(Workspace* ws) noexcept {
    State& s = st();
    if (s.owner == ws) s.owner = nullptr;
    if (s.ws == ws) s.ws = nullptr;
}
void reset_counters() noexcept { st().c = AllocCounters{}; }
AllocCounters counters() noexcept { return st().c; }

}  // namespace lume_e1

void* operator new(std::size_t n) { return lume_e1::allocate(n); }
void* operator new[](std::size_t n) { return lume_e1::allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return lume_e1::allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return lume_e1::allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { lume_e1::release(p); }
void operator delete[](void* p) noexcept { lume_e1::release(p); }
void operator delete(void* p, std::size_t) noexcept { lume_e1::release(p); }
void operator delete[](void* p, std::size_t) noexcept { lume_e1::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { lume_e1::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { lume_e1::release(p); }

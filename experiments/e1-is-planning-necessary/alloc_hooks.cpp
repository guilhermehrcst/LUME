#include "alloc_hooks.hpp"

#include <cstdlib>
#include <new>

namespace lume_e1 {
namespace {

// Live counted malloc blocks, by pointer. Frees are almost always LIFO, so the
// search runs from the back.
struct Live {
    void* p;
    std::size_t n;
};
constexpr std::size_t kTable = 1 << 14;

struct State {
    bool armed = false;
    Workspace* ws = nullptr;     // routing target for result-sized requests
    Workspace* owner = nullptr;  // most recent workspace: ownership checks on delete
    std::size_t bytes = 0;       // exact result size
    AllocCounters c;
    Live live[kTable];
    std::size_t live_count = 0;
};

State& st() noexcept {
    static State s;  // constant-initialized: safe before main
    return s;
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
        if (s.live_count < kTable) {
            s.live[s.live_count++] = {p, n};
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
    for (std::size_t i = s.live_count; i-- > 0;) {
        if (s.live[i].p == p) {
            if (s.armed) {
                s.c.live_bytes -= static_cast<std::int64_t>(s.live[i].n);
                ++s.c.result_frees;
            }
            s.live[i] = s.live[--s.live_count];
            break;
        }
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

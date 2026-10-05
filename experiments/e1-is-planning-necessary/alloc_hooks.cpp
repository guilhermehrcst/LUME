#include "alloc_hooks.hpp"

#include <cstdlib>
#include <new>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace lume_e1 {
namespace {

struct State {
    bool armed = false;
    Workspace* ws = nullptr;     // routing target for result-sized requests
    Workspace* owner = nullptr;  // most recent workspace: ownership checks on delete
    std::size_t min_bytes = 0;
    AllocCounters c;
};

State& st() noexcept {
    static State s;  // constant-initialized: safe before main
    return s;
}

// Bytes malloc really reserved for p (>= the request); 0 when unknown.
std::size_t usable(void* p) noexcept {
#if defined(__GLIBC__)
    return malloc_usable_size(p);
#else
    (void)p;
    return 0;
#endif
}

void* allocate(std::size_t n) {
    State& s = st();
    if (s.armed && n >= s.min_bytes) {
        ++s.c.result_allocs;
        s.c.result_alloc_bytes += n;
        if (s.ws != nullptr) {
            if (void* p = s.ws->allocate(n)) {
                ++s.c.arena_allocs;
                s.c.live_bytes += static_cast<std::int64_t>(n);
                if (s.c.live_bytes > s.c.peak_live_bytes) s.c.peak_live_bytes = s.c.live_bytes;
                return p;
            }
            ++s.c.arena_fallbacks;
        }
        void* p = std::malloc(n);
        if (p == nullptr) throw std::bad_alloc();
        s.c.live_bytes += static_cast<std::int64_t>(usable(p));
        if (s.c.live_bytes > s.c.peak_live_bytes) s.c.peak_live_bytes = s.c.live_bytes;
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
        // allocate() added the requested size to live_bytes.
        if (s.armed) {
            s.c.live_bytes -= static_cast<std::int64_t>(s.owner->requested_size(p));
            ++s.c.result_frees;
        }
        s.owner->deallocate(p);
        return;
    }
    if (s.armed && s.min_bytes != 0) {
        const std::size_t u = usable(p);
        if (u >= s.min_bytes) {
            s.c.live_bytes -= static_cast<std::int64_t>(u);
            ++s.c.result_frees;
        }
    }
    std::free(p);
}

}  // namespace

void arm(Workspace* ws, std::size_t min_bytes) noexcept {
    State& s = st();
    s.ws = ws;
    if (ws != nullptr) s.owner = ws;
    s.min_bytes = min_bytes;
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

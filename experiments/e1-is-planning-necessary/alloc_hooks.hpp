#pragma once

// Allocation instrumentation and optional workspace routing for E1. The
// definitions replace the global operator new/delete in alloc_hooks.cpp, so
// link that translation unit into exactly the executables that want it.
//
// While armed, requests of exactly `bytes` ("result-sized": every Lume result
// buffer is 4*N bytes) are counted and, when a workspace is attached, served
// from it. Everything else (the executor's index vectors, any request while
// disarmed) goes to malloc untouched. Matching is by exact size and the live
// set of counted malloc blocks is tracked by pointer, so a bookkeeping vector
// that happens to be large cannot be mistaken for a result and sizes are never
// guessed from malloc_usable_size.

#include <cstddef>
#include <cstdint>

#include "workspace.hpp"

namespace lume_e1 {

struct AllocCounters {
    std::uint64_t result_allocs = 0;       // armed requests >= min_bytes
    std::uint64_t result_alloc_bytes = 0;  // sum of requested sizes
    std::uint64_t arena_allocs = 0;        // of those, served by the workspace
    std::uint64_t arena_fallbacks = 0;     // workspace exhausted, served by malloc
    std::uint64_t result_frees = 0;
    std::int64_t live_bytes = 0;           // result-sized bytes live since the last reset
    std::int64_t peak_live_bytes = 0;
    std::uint64_t table_overflows = 0;     // counted malloc blocks that could not be tracked (invalidates live_bytes)
};

// Start counting; route result-sized requests to `ws` when non-null. Every
// workspace ever passed here keeps ownership checks on deletes until
// forget(ws), so a block is never handed to free() even if the routing was
// switched off before it was released.
void arm(Workspace* ws, std::size_t bytes) noexcept;
void forget(Workspace* ws) noexcept;
void disarm() noexcept;
void reset_counters() noexcept;
[[nodiscard]] AllocCounters counters() noexcept;

}  // namespace lume_e1

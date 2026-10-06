// E1 workspace tests: alignment, capacity boundary, no overlap of live blocks,
// reset and reuse, repeated runs, small and large sizes, page release.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.hpp"
#include "workspace.hpp"

using lume_e1::Workspace;

namespace {

bool aligned(const void* p) { return reinterpret_cast<std::uintptr_t>(p) % Workspace::kAlign == 0; }

void alignment_and_small_sizes() {
    Workspace ws(1 << 20);
    LUME_CHECK(ws.valid());
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{4}, std::size_t{63},
                                std::size_t{64}, std::size_t{65}, std::size_t{4096}, std::size_t{4099}}) {
        void* p = ws.allocate(n);
        if (!LUME_CHECK(p != nullptr)) continue;
        LUME_CHECK(aligned(p));
        LUME_CHECK(ws.owns(p));
        std::memset(p, 0x5a, n == 0 ? 1 : n);  // the whole requested payload is writable
        ws.deallocate(p);
    }
    LUME_CHECK(ws.live_blocks() == 0);
}

void capacity_boundary() {
    // Exactly-fitting blocks: header(64) + payload(64) = 128 bytes each.
    constexpr std::size_t block = Workspace::kHeader + Workspace::kAlign;
    constexpr std::size_t count = 32;
    Workspace ws(block * count);  // 4096 bytes: a whole number of pages
    LUME_CHECK(ws.capacity() == block * count);
    std::vector<void*> blocks;
    for (std::size_t i = 0; i < count; ++i) {
        void* p = ws.allocate(64);
        if (!LUME_CHECK(p != nullptr)) break;
        blocks.push_back(p);
    }
    LUME_CHECK(blocks.size() == count);
    LUME_CHECK(ws.allocate(1) == nullptr);            // one block past the end is refused, not overrun
    LUME_CHECK(ws.allocate(1u << 30) == nullptr);     // far too large
    LUME_CHECK(ws.allocate(~std::size_t{0}) == nullptr);  // arithmetic overflow is refused
    LUME_CHECK(ws.live_blocks() == count);
    // A freed block of the same size can be handed out again even when full.
    ws.deallocate(blocks.back());
    void* again = ws.allocate(64);
    LUME_CHECK(again == blocks.back());
    for (void* p : blocks) {
        if (p == blocks.back()) ws.deallocate(again);
        else ws.deallocate(p);
    }
    LUME_CHECK(ws.live_blocks() == 0);
    // Freed small blocks are not merged: a larger request cannot be satisfied from them.
    LUME_CHECK(ws.allocate(128) == nullptr);
    LUME_CHECK(ws.reset());
    LUME_CHECK(ws.allocate(128) != nullptr);  // after reset the whole region is available again
}

void no_overlap_between_live_blocks() {
    Workspace ws(8 << 20);
    struct Rec {
        unsigned char* p;
        std::size_t n;
        unsigned char tag;
    };
    std::vector<Rec> live;
    const std::size_t sizes[] = {1, 100, 4096, 4099 * 4, 65536, 3, 64, 1 << 16};
    unsigned char tag = 1;
    for (int round = 0; round < 3; ++round) {
        for (const std::size_t n : sizes) {
            auto* p = static_cast<unsigned char*>(ws.allocate(n));
            if (!LUME_CHECK(p != nullptr)) return;
            std::memset(p, tag, n);
            live.push_back({p, n, tag++});
        }
        // Free every other block mid-way to force recycling while others stay live.
        for (std::size_t i = 0; i < live.size(); i += 2) {
            if (live[i].p != nullptr) {
                ws.deallocate(live[i].p);
                live[i].p = nullptr;
            }
        }
    }
    // Every still-live block still holds exactly its own tag: nothing overwrote it.
    for (const Rec& r : live) {
        if (r.p == nullptr) continue;
        for (std::size_t i = 0; i < r.n; ++i) {
            if (r.p[i] != r.tag) {
                LUME_CHECK(false);
                break;
            }
        }
    }
    // And the live blocks are pairwise disjoint (header included).
    for (std::size_t i = 0; i < live.size(); ++i) {
        for (std::size_t j = i + 1; j < live.size(); ++j) {
            if (live[i].p == nullptr || live[j].p == nullptr) continue;
            const auto a0 = reinterpret_cast<std::uintptr_t>(live[i].p) - Workspace::kHeader;
            const auto a1 = reinterpret_cast<std::uintptr_t>(live[i].p) + live[i].n;
            const auto b0 = reinterpret_cast<std::uintptr_t>(live[j].p) - Workspace::kHeader;
            const auto b1 = reinterpret_cast<std::uintptr_t>(live[j].p) + live[j].n;
            LUME_CHECK(a1 <= b0 || b1 <= a0);
        }
    }
    for (const Rec& r : live) {
        if (r.p != nullptr) ws.deallocate(r.p);
    }
    LUME_CHECK(ws.live_blocks() == 0 && ws.live_payload_bytes() == 0);
}

void reset_reuse_and_repeated_runs() {
    Workspace ws(4 << 20);
    std::vector<void*> first;
    for (int run = 0; run < 5; ++run) {
        LUME_CHECK(ws.reset());
        std::vector<void*> got;
        for (const std::size_t n : {std::size_t{16384}, std::size_t{16384}, std::size_t{4096}}) {
            void* p = ws.allocate(n);
            LUME_CHECK(p != nullptr);
            got.push_back(p);
        }
        if (run == 0) first = got;
        LUME_CHECK(got == first);  // identical addresses after every reset
        // LIFO recycling inside a run: free then allocate the same size returns the same block.
        ws.deallocate(got[1]);
        LUME_CHECK(ws.allocate(16384) == got[1]);
        for (void* p : got) ws.deallocate(p);
    }
    // reset() refuses while a block is live, and changes nothing.
    void* p = ws.allocate(100);
    LUME_CHECK(!ws.reset());
    LUME_CHECK(ws.live_blocks() == 1);
    LUME_CHECK(!ws.release_pages());
    LUME_CHECK(!ws.prefault(4096));
    ws.deallocate(p);
    LUME_CHECK(ws.reset());
}

void large_and_release() {
    // 256 MiB of address space reserved; only what is touched costs memory.
    Workspace ws(256u << 20);
    LUME_CHECK(ws.valid());
    constexpr std::size_t n = 64u << 20;  // 64 MiB payload
    auto* p = static_cast<unsigned char*>(ws.allocate(n));
    if (!LUME_CHECK(p != nullptr)) return;
    LUME_CHECK(aligned(p));
    p[0] = 7;
    p[n - 1] = 9;
    p[n / 2] = 11;
    ws.deallocate(p);
    LUME_CHECK(ws.reset());
    // release_pages gives the used range back: untouched-again memory reads as zero.
    LUME_CHECK(ws.release_pages());
    auto* q = static_cast<unsigned char*>(ws.allocate(n));
    LUME_CHECK(q == p);
    LUME_CHECK(q[n - 1] == 0 && q[n / 2] == 0);
    ws.deallocate(q);
    LUME_CHECK(ws.reset());
    // prefault never exceeds the capacity.
    LUME_CHECK(ws.prefault(~std::size_t{0} / 2));
    LUME_CHECK(ws.high_water() == ws.capacity());
}

void many_size_classes_are_safe() {
    // More distinct sizes than recycling slots: allocation still works, the
    // overflow classes simply are not recycled until reset().
    Workspace ws(8 << 20);
    std::vector<void*> blocks;
    for (std::size_t i = 1; i <= 40; ++i) {
        void* p = ws.allocate(i * 64);
        LUME_CHECK(p != nullptr);
        blocks.push_back(p);
    }
    for (void* p : blocks) ws.deallocate(p);
    LUME_CHECK(ws.live_blocks() == 0);
    LUME_CHECK(ws.reset());
}

void zero_capacity_is_inert() {
    Workspace ws(0);
    LUME_CHECK(ws.allocate(1) == nullptr);
    LUME_CHECK(!ws.owns(&ws));
}

}  // namespace

int main() {
    alignment_and_small_sizes();
    capacity_boundary();
    no_overlap_between_live_blocks();
    reset_reuse_and_repeated_runs();
    large_and_release();
    many_size_classes_are_safe();
    zero_capacity_is_inert();
    return lume_test::finish("e1_workspace");
}

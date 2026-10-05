#pragma once

// E1 workspace (arena) control. Linux only; single-threaded.
//
// One anonymous mapping, reserved up front (MAP_NORESERVE, so untouched pages
// cost nothing) and carved into 64-byte-aligned blocks by a bump pointer.
// Freed blocks go back to a per-size LIFO list and are handed out again, which
// mirrors how malloc recycles a just-freed buffer (a plain bump arena would
// instead spread K back-to-back calls over K times the memory). reset()
// rewinds the bump pointer and forgets every free list; it is only legal when
// no block is live.
//
// Fail-closed rules: allocate() returns nullptr instead of overrunning the
// region; deallocate() aborts on a pointer it does not own or a double free;
// reset() and release_pages() refuse (return false, change nothing) while
// blocks are live. The workspace never calls operator new, so it is safe to
// use from a replaced global operator new.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <sys/mman.h>

namespace lume_e1 {

class Workspace {
public:
    static constexpr std::size_t kAlign = 64;           // payload and header alignment
    static constexpr std::size_t kHeader = 64;          // bytes before every payload
    static constexpr std::size_t kPage = 4096;
    static constexpr std::size_t kMaxClasses = 16;      // distinct block sizes that can be recycled

    explicit Workspace(std::size_t capacity_bytes) {
        capacity_ = round_up(capacity_bytes, kPage);
        void* p = ::mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        base_ = p == MAP_FAILED ? nullptr : static_cast<unsigned char*>(p);
        if (base_ == nullptr) capacity_ = 0;
    }
    ~Workspace() {
        if (base_ != nullptr) ::munmap(base_, capacity_);
    }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t high_water() const noexcept { return bump_high_; }
    [[nodiscard]] std::size_t live_blocks() const noexcept { return live_blocks_; }
    [[nodiscard]] std::size_t live_payload_bytes() const noexcept { return live_payload_; }
    [[nodiscard]] std::uint64_t allocations() const noexcept { return allocations_; }
    [[nodiscard]] std::uint64_t recycled() const noexcept { return recycled_; }

    // Bytes originally requested for a live block (0 for a pointer it does not own).
    [[nodiscard]] std::size_t requested_size(const void* p) const noexcept {
        if (!owns(p)) return 0;
        return static_cast<std::size_t>(
            reinterpret_cast<const Header*>(static_cast<const unsigned char*>(p) - kHeader)->requested);
    }

    [[nodiscard]] bool owns(const void* p) const noexcept {
        const auto* c = static_cast<const unsigned char*>(p);
        return base_ != nullptr && c >= base_ + kHeader && c < base_ + capacity_;
    }

    // Payload of at least `bytes` bytes (0 is treated as 1), 64-byte aligned;
    // nullptr when the region cannot hold it.
    [[nodiscard]] void* allocate(std::size_t bytes) noexcept {
        if (base_ == nullptr) return nullptr;
        if (bytes == 0) bytes = 1;
        if (bytes > capacity_) return nullptr;  // also guards the round_up below against overflow
        const std::size_t total = kHeader + round_up(bytes, kAlign);
        Header* h = pop_free(total);
        if (h != nullptr) {
            ++recycled_;
        } else {
            if (total > capacity_ - bump_) return nullptr;
            h = reinterpret_cast<Header*>(base_ + bump_);
            bump_ += total;
            if (bump_ > bump_high_) bump_high_ = bump_;
            h->total = total;
        }
        h->requested = bytes;
        h->state = kLive;
        h->next = nullptr;
        ++allocations_;
        ++live_blocks_;
        live_payload_ += bytes;
        return reinterpret_cast<unsigned char*>(h) + kHeader;
    }

    void deallocate(void* p) noexcept {
        if (!owns(p)) std::abort();
        auto* h = reinterpret_cast<Header*>(static_cast<unsigned char*>(p) - kHeader);
        if (h->state != kLive) std::abort();  // double free or foreign pointer
        h->state = kFree;
        --live_blocks_;
        live_payload_ -= h->requested;
        push_free(h);
    }

    // Rewind to empty. Refused (false) while any block is live.
    bool reset() noexcept {
        if (live_blocks_ != 0) return false;
        bump_ = 0;
        for (auto& c : classes_) c = SizeClass{};
        return true;
    }

    // Write one byte per page over the first `bytes` of the region so that the
    // kernel has materialized them. Only legal when empty.
    bool prefault(std::size_t bytes) noexcept {
        if (live_blocks_ != 0 || base_ == nullptr) return false;
        if (bytes > capacity_) bytes = capacity_;
        for (std::size_t off = 0; off < bytes; off += kPage) {
            volatile unsigned char* q = base_ + off;
            *q = 0;
        }
        if (bytes > bump_high_) bump_high_ = bytes;  // counted as used for release_pages
        return true;
    }

    // Give every page ever used back to the kernel (MADV_DONTNEED on an
    // anonymous private mapping: later reads see zeros, the next write faults a
    // fresh zero page). Only legal when empty, because it destroys contents.
    bool release_pages() noexcept {
        if (live_blocks_ != 0 || base_ == nullptr || bump_ != 0) return false;
        const std::size_t used = round_up(bump_high_, kPage);
        if (used == 0) return true;
        return ::madvise(base_, used, MADV_DONTNEED) == 0;
    }

private:
    static constexpr std::uint64_t kLive = 0xA110C8EDA110C8EDull;
    static constexpr std::uint64_t kFree = 0xF2EEF2EEF2EEF2EEull;

    struct alignas(kAlign) Header {
        std::uint64_t total;      // header + rounded payload
        std::uint64_t requested;  // bytes asked for
        std::uint64_t state;
        Header* next;             // free-list link
    };
    static_assert(sizeof(Header) == kHeader);

    struct SizeClass {
        std::size_t total = 0;
        Header* head = nullptr;
    };

    static constexpr std::size_t round_up(std::size_t v, std::size_t a) noexcept { return (v + a - 1) / a * a; }

    Header* pop_free(std::size_t total) noexcept {
        for (auto& c : classes_) {
            if (c.total == total && c.head != nullptr) {
                Header* h = c.head;
                c.head = h->next;
                return h;
            }
        }
        return nullptr;
    }
    void push_free(Header* h) noexcept {
        SizeClass* slot = nullptr;
        for (auto& c : classes_) {
            if (c.total == h->total) {
                slot = &c;
                break;
            }
            if (slot == nullptr && c.total == 0) slot = &c;
        }
        if (slot == nullptr) return;  // class table full: the block stays unavailable until reset()
        slot->total = h->total;
        h->next = slot->head;
        slot->head = h;
    }

    unsigned char* base_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t bump_ = 0;
    std::size_t bump_high_ = 0;
    std::size_t live_blocks_ = 0;
    std::size_t live_payload_ = 0;
    std::uint64_t allocations_ = 0;
    std::uint64_t recycled_ = 0;
    SizeClass classes_[kMaxClasses]{};
};

}  // namespace lume_e1

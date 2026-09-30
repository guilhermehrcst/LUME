#pragma once

// The three layouts of experiment 002 (README section 4):
//   TypedColumnar   (B1) typed columns, no encoding;
//   DictColumnar    (B2) B1 plus dictionaries with byte-aligned indices;
//   EncodedColumnar (E1) dictionary + bit packing + delta, lossless.
//
// Every layout can rebuild any logical row (row()) and answers the same
// queries (s1, s2, s3). Footprints are exact: every buffer needed to read the
// data back is counted.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "workload.hpp"

namespace exp002 {

static_assert(std::endian::native == std::endian::little, "bit packing assumes a little-endian host");

// ---------------------------------------------------------------- bit tools

inline constexpr std::size_t kPad = 16;  // zero bytes after every packed buffer (read_bits reads ahead)

[[nodiscard]] inline unsigned width_of(u64 v) noexcept { return static_cast<unsigned>(std::bit_width(v)); }

[[nodiscard]] inline u64 zigzag(i64 x) noexcept {
    return (static_cast<u64>(x) << 1) ^ static_cast<u64>(x >> 63);
}
[[nodiscard]] inline i64 unzigzag(u64 u) noexcept { return static_cast<i64>((u >> 1) ^ (u64{0} - (u & 1))); }

[[nodiscard]] inline u64 read_bits(const std::uint8_t* base, u64 bitpos, unsigned w) noexcept {
    if (w == 0) return 0;
    const u64 byte = bitpos >> 3;
    const unsigned sh = static_cast<unsigned>(bitpos & 7);
    u64 lo = 0;
    std::memcpy(&lo, base + byte, 8);
    u64 v = lo >> sh;
    if (w + sh > 64) v |= static_cast<u64>(base[byte + 8]) << (64 - sh);
    return w == 64 ? v : (v & ((u64{1} << w) - 1));
}

// `value` must fit in `w` bits. The buffer must be zero-initialized and padded.
inline void write_bits(std::uint8_t* base, u64 bitpos, unsigned w, u64 value) noexcept {
    if (w == 0) return;
    const u64 byte = bitpos >> 3;
    const unsigned sh = static_cast<unsigned>(bitpos & 7);
    u64 lo = 0;
    std::memcpy(&lo, base + byte, 8);
    lo |= value << sh;
    std::memcpy(base + byte, &lo, 8);
    if (w + sh > 64) base[byte + 8] = static_cast<std::uint8_t>(base[byte + 8] | (value >> (64 - sh)));
}

// Fixed bit width for every element: direct addressing.
class FixedPacked {
public:
    static FixedPacked pack(std::span<const u64> values, unsigned width) {
        if (width > 64) throw std::invalid_argument("FixedPacked: width > 64");
        FixedPacked p;
        p.n_ = values.size();
        p.w_ = width;
        p.bytes_.assign((values.size() * width + 7) / 8 + kPad, 0);
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (width < 64 && (values[i] >> width) != 0) throw std::invalid_argument("FixedPacked: value exceeds width");
            write_bits(p.bytes_.data(), static_cast<u64>(i) * width, width, values[i]);
        }
        return p;
    }

    [[nodiscard]] u64 get(std::size_t i) const noexcept { return read_bits(bytes_.data(), static_cast<u64>(i) * w_, w_); }
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] unsigned width() const noexcept { return w_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_.size(); }

private:
    std::size_t n_ = 0;
    unsigned w_ = 0;
    std::vector<std::uint8_t> bytes_;
};

// Blocks of kBlock values, each with its own bit width (the width of the
// largest value in the block). Random access through a per-block byte offset.
class BlockPacked {
public:
    static constexpr std::size_t kBlock = 128;

    static BlockPacked pack(std::span<const u64> values) {
        BlockPacked p;
        p.n_ = values.size();
        const std::size_t blocks = (values.size() + kBlock - 1) / kBlock;
        p.widths_.resize(blocks);
        p.offsets_.resize(blocks + 1);
        u64 off = 0;
        for (std::size_t b = 0; b < blocks; ++b) {
            const std::size_t first = b * kBlock;
            const std::size_t rows = std::min(kBlock, values.size() - first);
            u64 acc = 0;
            for (std::size_t j = 0; j < rows; ++j) acc |= values[first + j];
            const unsigned w = width_of(acc);
            if (off > std::numeric_limits<u32>::max()) throw std::length_error("BlockPacked: column too large");
            p.widths_[b] = static_cast<std::uint8_t>(w);
            p.offsets_[b] = static_cast<u32>(off);
            off += (static_cast<u64>(rows) * w + 7) / 8;
        }
        if (off > std::numeric_limits<u32>::max()) throw std::length_error("BlockPacked: column too large");
        p.offsets_[blocks] = static_cast<u32>(off);
        p.bytes_.assign(static_cast<std::size_t>(off) + kPad, 0);
        for (std::size_t b = 0; b < blocks; ++b) {
            const std::size_t first = b * kBlock;
            const std::size_t rows = std::min(kBlock, values.size() - first);
            const unsigned w = p.widths_[b];
            std::uint8_t* base = p.bytes_.data() + p.offsets_[b];
            for (std::size_t j = 0; j < rows; ++j) write_bits(base, static_cast<u64>(j) * w, w, values[first + j]);
        }
        return p;
    }

    [[nodiscard]] u64 get(std::size_t i) const noexcept {
        const std::size_t b = i / kBlock;
        const unsigned w = widths_[b];
        return read_bits(bytes_.data() + offsets_[b], static_cast<u64>(i % kBlock) * w, w);
    }

    // f(first_index, const u64* values, count), one call per block, in order.
    template <class F>
    void for_each_block(F&& f) const {
        u64 buf[kBlock];
        for (std::size_t b = 0; b < widths_.size(); ++b) {
            const unsigned w = widths_[b];
            const std::uint8_t* base = bytes_.data() + offsets_[b];
            const std::size_t first = b * kBlock;
            const std::size_t rows = std::min(kBlock, n_ - first);
            for (std::size_t j = 0; j < rows; ++j) buf[j] = read_bits(base, static_cast<u64>(j) * w, w);
            f(first, static_cast<const u64*>(buf), rows);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] std::size_t bytes() const noexcept {
        return bytes_.size() + widths_.size() * sizeof(std::uint8_t) + offsets_.size() * sizeof(u32);
    }

private:
    std::size_t n_ = 0;
    std::vector<std::uint8_t> widths_;
    std::vector<u32> offsets_;
    std::vector<std::uint8_t> bytes_;
};

[[nodiscard]] inline unsigned bits_for_card(std::size_t card) noexcept {
    return card <= 1 ? 0U : width_of(static_cast<u64>(card - 1));
}

[[nodiscard]] inline std::vector<u64> widen(const std::vector<u32>& v) { return std::vector<u64>(v.begin(), v.end()); }

// ---------------------------------------------------------------- strings

// Distinct strings in one byte buffer plus offsets.
struct StringDict {
    std::vector<char> bytes;
    std::vector<u32> offsets{0};

    [[nodiscard]] std::size_t card() const noexcept { return offsets.size() - 1; }
    [[nodiscard]] std::string_view at(std::size_t code) const noexcept {
        return {bytes.data() + offsets[code], static_cast<std::size_t>(offsets[code + 1] - offsets[code])};
    }
    // card() when absent.
    [[nodiscard]] std::size_t find(std::string_view s) const noexcept {
        for (std::size_t c = 0; c < card(); ++c) {
            if (at(c) == s) return c;
        }
        return card();
    }
    [[nodiscard]] std::size_t footprint() const noexcept { return bytes.size() + offsets.size() * sizeof(u32); }
};

struct DictEncoded {
    StringDict dict;
    std::vector<u32> codes;
};

[[nodiscard]] inline DictEncoded dictionary_encode(const StringColumn& col, std::size_t n) {
    DictEncoded out;
    out.codes.reserve(n);
    std::unordered_map<std::string_view, u32> seen;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string_view s = col.at(i);
        const auto [it, inserted] = seen.try_emplace(s, static_cast<u32>(out.dict.card()));
        if (inserted) {
            out.dict.bytes.insert(out.dict.bytes.end(), s.begin(), s.end());
            if (out.dict.bytes.size() > std::numeric_limits<u32>::max()) throw std::length_error("dictionary too large");
            out.dict.offsets.push_back(static_cast<u32>(out.dict.bytes.size()));
        }
        out.codes.push_back(it->second);
    }
    return out;
}

// Variable-length strings the way a plain columnar format stores them.
struct ArrowStrings {
    std::vector<u32> offsets{0};
    std::vector<char> bytes;

    static ArrowStrings from(const StringColumn& col, std::size_t n) {
        ArrowStrings a;
        a.offsets.reserve(n + 1);
        for (std::size_t i = 0; i < n; ++i) {
            const std::string_view s = col.at(i);
            a.bytes.insert(a.bytes.end(), s.begin(), s.end());
            if (a.bytes.size() > std::numeric_limits<u32>::max()) throw std::length_error("string column too large");
            a.offsets.push_back(static_cast<u32>(a.bytes.size()));
        }
        return a;
    }
    [[nodiscard]] std::string_view at(std::size_t i) const noexcept {
        return {bytes.data() + offsets[i], static_cast<std::size_t>(offsets[i + 1] - offsets[i])};
    }
    [[nodiscard]] std::size_t footprint() const noexcept { return bytes.size() + offsets.size() * sizeof(u32); }
};

// ---------------------------------------------------------------- byte-aligned columns

// Signed integers in the narrowest of int8/16/32/64 that holds the whole column.
class NarrowInt {
public:
    static NarrowInt from(std::span<const i64> v) {
        i64 lo = 0;
        i64 hi = 0;
        if (!v.empty()) {
            lo = hi = v[0];
            for (const i64 x : v) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        }
        NarrowInt r;
        if (lo >= std::numeric_limits<std::int8_t>::min() && hi <= std::numeric_limits<std::int8_t>::max()) {
            r.data_ = convert<std::int8_t>(v);
        } else if (lo >= std::numeric_limits<std::int16_t>::min() && hi <= std::numeric_limits<std::int16_t>::max()) {
            r.data_ = convert<std::int16_t>(v);
        } else if (lo >= std::numeric_limits<std::int32_t>::min() && hi <= std::numeric_limits<std::int32_t>::max()) {
            r.data_ = convert<std::int32_t>(v);
        } else {
            r.data_ = std::vector<i64>(v.begin(), v.end());
        }
        return r;
    }

    [[nodiscard]] i64 get(std::size_t i) const noexcept {
        return std::visit([i](const auto& x) { return static_cast<i64>(x[i]); }, data_);
    }
    [[nodiscard]] std::size_t bytes() const noexcept {
        return std::visit(
            [](const auto& x) { return x.size() * sizeof(typename std::decay_t<decltype(x)>::value_type); }, data_);
    }

private:
    template <class T>
    static std::vector<T> convert(std::span<const i64> v) {
        std::vector<T> out(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) out[i] = static_cast<T>(v[i]);
        return out;
    }
    std::variant<std::vector<std::int8_t>, std::vector<std::int16_t>, std::vector<std::int32_t>, std::vector<i64>> data_;
};

// Dictionary codes in the narrowest of uint8/16/32.
class NarrowUInt {
public:
    static NarrowUInt from(const std::vector<u32>& codes, std::size_t card) {
        NarrowUInt r;
        if (card <= 256) {
            r.data_ = convert<std::uint8_t>(codes);
        } else if (card <= 65536) {
            r.data_ = convert<std::uint16_t>(codes);
        } else {
            r.data_ = codes;
        }
        return r;
    }

    [[nodiscard]] u64 get(std::size_t i) const noexcept {
        return std::visit([i](const auto& x) { return static_cast<u64>(x[i]); }, data_);
    }
    template <class F>
    decltype(auto) visit(F&& f) const {
        return std::visit(std::forward<F>(f), data_);
    }
    [[nodiscard]] std::size_t bytes() const noexcept {
        return std::visit(
            [](const auto& x) { return x.size() * sizeof(typename std::decay_t<decltype(x)>::value_type); }, data_);
    }

private:
    template <class T>
    static std::vector<T> convert(const std::vector<u32>& v) {
        std::vector<T> out(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) out[i] = static_cast<T>(v[i]);
        return out;
    }
    std::variant<std::vector<std::uint8_t>, std::vector<std::uint16_t>, std::vector<u32>> data_;
};

struct IdHash {
    std::size_t operator()(const Id128& a) const noexcept {
        return static_cast<std::size_t>(mix(mix(0x1234567ull, a[0]), a[1]));
    }
};

// ---------------------------------------------------------------- reporting

struct Breakdown {
    std::vector<std::pair<std::string, std::size_t>> parts;

    void add(std::string name, std::size_t bytes) { parts.emplace_back(std::move(name), bytes); }
    [[nodiscard]] std::size_t total() const noexcept {
        std::size_t t = 0;
        for (const auto& p : parts) t += p.second;
        return t;
    }
    [[nodiscard]] std::size_t bytes_of(std::string_view name) const noexcept {
        for (const auto& p : parts) {
            if (p.first == name) return p.second;
        }
        return 0;
    }
};

// Shared query definitions (README section 6). All arithmetic is wrapping uint64.
//   s1 = count(*) where event_name == target
//   s2 = sum(received - occurred) where event_name == target
//   s3 = max(occurred)   (INT64_MIN for an empty table)

// ---------------------------------------------------------------- B1

class TypedColumnar {
public:
    static TypedColumnar build(const Workload& w) {
        const std::size_t n = w.size();
        TypedColumnar t;
        t.n_ = n;
        t.event_id_ = w.event_id;
        t.session_id_ = w.session_id;
        t.seq_ = NarrowInt::from(w.client_seq);
        const std::vector<i64> sv(w.schema_version.begin(), w.schema_version.end());
        t.schema_version_ = NarrowInt::from(sv);
        t.occurred_ = w.occurred;
        t.received_ = w.received;
        t.mode_ = ArrowStrings::from(w.mode, n);
        t.event_name_ = ArrowStrings::from(w.event_name, n);
        t.route_ = ArrowStrings::from(w.route, n);
        t.acquisition_ = ArrowStrings::from(w.acquisition, n);
        t.properties_ = ArrowStrings::from(w.properties, n);
        return t;
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

    [[nodiscard]] RowView row(std::size_t i) const noexcept {
        return RowView{event_id_[i],   session_id_[i], seq_.get(i),          static_cast<u32>(schema_version_.get(i)),
                       occurred_[i],   received_[i],   mode_.at(i),          event_name_.at(i),
                       route_.at(i),   acquisition_.at(i), properties_.at(i)};
    }

    [[nodiscard]] u64 s1(std::string_view target) const noexcept {
        u64 count = 0;
        for (std::size_t i = 0; i < n_; ++i) count += static_cast<u64>(event_name_.at(i) == target);
        return count;
    }
    [[nodiscard]] u64 s2(std::string_view target) const noexcept {
        u64 acc = 0;
        for (std::size_t i = 0; i < n_; ++i) {
            if (event_name_.at(i) == target) acc += static_cast<u64>(received_[i]) - static_cast<u64>(occurred_[i]);
        }
        return acc;
    }
    [[nodiscard]] i64 s3() const noexcept {
        i64 best = std::numeric_limits<i64>::min();
        for (const i64 t : occurred_) best = std::max(best, t);
        return best;
    }

    [[nodiscard]] Breakdown breakdown() const {
        Breakdown b;
        b.add("event_id", event_id_.size() * sizeof(Id128));
        b.add("session_id", session_id_.size() * sizeof(Id128));
        b.add("client_seq", seq_.bytes());
        b.add("schema_version", schema_version_.bytes());
        b.add("occurred_at", occurred_.size() * sizeof(i64));
        b.add("received_at", received_.size() * sizeof(i64));
        b.add("mode", mode_.footprint());
        b.add("event_name", event_name_.footprint());
        b.add("route", route_.footprint());
        b.add("acquisition", acquisition_.footprint());
        b.add("properties", properties_.footprint());
        return b;
    }

private:
    std::size_t n_ = 0;
    std::vector<Id128> event_id_;
    std::vector<Id128> session_id_;
    NarrowInt seq_;
    NarrowInt schema_version_;
    std::vector<i64> occurred_;
    std::vector<i64> received_;
    ArrowStrings mode_, event_name_, route_, acquisition_, properties_;
};

// ---------------------------------------------------------------- B2

class DictColumnar {
public:
    static DictColumnar build(const Workload& w) {
        const std::size_t n = w.size();
        DictColumnar d;
        d.n_ = n;
        d.event_id_ = w.event_id;
        {
            std::unordered_map<Id128, u32, IdHash> seen;
            std::vector<u32> codes;
            codes.reserve(n);
            for (const Id128& id : w.session_id) {
                const auto [it, inserted] = seen.try_emplace(id, static_cast<u32>(d.session_ids_.size()));
                if (inserted) d.session_ids_.push_back(id);
                codes.push_back(it->second);
            }
            d.session_code_ = NarrowUInt::from(codes, d.session_ids_.size());
        }
        d.seq_ = NarrowInt::from(w.client_seq);
        const std::vector<i64> sv(w.schema_version.begin(), w.schema_version.end());
        d.schema_version_ = NarrowInt::from(sv);
        d.occurred_ = w.occurred;
        d.received_ = w.received;
        d.mode_ = make_col(w.mode, n);
        d.event_name_ = make_col(w.event_name, n);
        d.route_ = make_col(w.route, n);
        d.acquisition_ = make_col(w.acquisition, n);
        d.properties_ = make_col(w.properties, n);
        return d;
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

    [[nodiscard]] RowView row(std::size_t i) const noexcept {
        return RowView{event_id_[i],
                       session_ids_[session_code_.get(i)],
                       seq_.get(i),
                       static_cast<u32>(schema_version_.get(i)),
                       occurred_[i],
                       received_[i],
                       text(mode_, i),
                       text(event_name_, i),
                       text(route_, i),
                       text(acquisition_, i),
                       text(properties_, i)};
    }

    [[nodiscard]] u64 s1(std::string_view target) const noexcept {
        const std::size_t code = event_name_.dict.find(target);
        if (code == event_name_.dict.card()) return 0;
        return event_name_.codes.visit([&](const auto& v) {
            using T = typename std::decay_t<decltype(v)>::value_type;
            const T c = static_cast<T>(code);
            u64 count = 0;
            for (const T x : v) count += static_cast<u64>(x == c);
            return count;
        });
    }
    [[nodiscard]] u64 s2(std::string_view target) const noexcept {
        const std::size_t code = event_name_.dict.find(target);
        if (code == event_name_.dict.card()) return 0;
        return event_name_.codes.visit([&](const auto& v) {
            using T = typename std::decay_t<decltype(v)>::value_type;
            const T c = static_cast<T>(code);
            u64 acc = 0;
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (v[i] == c) acc += static_cast<u64>(received_[i]) - static_cast<u64>(occurred_[i]);
            }
            return acc;
        });
    }
    [[nodiscard]] i64 s3() const noexcept {
        i64 best = std::numeric_limits<i64>::min();
        for (const i64 t : occurred_) best = std::max(best, t);
        return best;
    }

    [[nodiscard]] Breakdown breakdown() const {
        Breakdown b;
        b.add("event_id", event_id_.size() * sizeof(Id128));
        b.add("session_id", session_ids_.size() * sizeof(Id128) + session_code_.bytes());
        b.add("client_seq", seq_.bytes());
        b.add("schema_version", schema_version_.bytes());
        b.add("occurred_at", occurred_.size() * sizeof(i64));
        b.add("received_at", received_.size() * sizeof(i64));
        b.add("mode", mode_.dict.footprint() + mode_.codes.bytes());
        b.add("event_name", event_name_.dict.footprint() + event_name_.codes.bytes());
        b.add("route", route_.dict.footprint() + route_.codes.bytes());
        b.add("acquisition", acquisition_.dict.footprint() + acquisition_.codes.bytes());
        b.add("properties", properties_.dict.footprint() + properties_.codes.bytes());
        return b;
    }

private:
    struct Col {
        StringDict dict;
        NarrowUInt codes;
    };
    static Col make_col(const StringColumn& c, std::size_t n) {
        DictEncoded e = dictionary_encode(c, n);
        Col col;
        col.codes = NarrowUInt::from(e.codes, e.dict.card());
        col.dict = std::move(e.dict);
        return col;
    }
    static std::string_view text(const Col& c, std::size_t i) noexcept { return c.dict.at(c.codes.get(i)); }

    std::size_t n_ = 0;
    std::vector<Id128> event_id_;
    std::vector<Id128> session_ids_;
    NarrowUInt session_code_;
    NarrowInt seq_;
    NarrowInt schema_version_;
    std::vector<i64> occurred_;
    std::vector<i64> received_;
    Col mode_, event_name_, route_, acquisition_, properties_;
};

// ---------------------------------------------------------------- E1

class EncodedColumnar {
public:
    static EncodedColumnar build(const Workload& w) {
        const std::size_t n = w.size();
        if (n > std::numeric_limits<u32>::max()) throw std::length_error("EncodedColumnar: too many rows");
        EncodedColumnar e;
        e.n_ = n;
        e.event_id_ = w.event_id;

        std::vector<u64> seq_gap(n);
        std::vector<u64> occ_delta(n);
        std::vector<u64> latency(n);
        for (std::size_t i = 0; i < n; ++i) {
            const bool first = i == 0 || w.session_id[i] != w.session_id[i - 1];
            if (first) {
                e.run_start_.push_back(static_cast<u32>(i));
                e.run_session_.push_back(w.session_id[i]);
                e.run_first_occ_.push_back(w.occurred[i]);
            }
            const u64 expected = first ? 0 : static_cast<u64>(w.client_seq[i - 1]) + 1;
            seq_gap[i] = zigzag(static_cast<i64>(static_cast<u64>(w.client_seq[i]) - expected));
            occ_delta[i] = first ? 0
                                 : zigzag(static_cast<i64>(static_cast<u64>(w.occurred[i]) -
                                                           static_cast<u64>(w.occurred[i - 1])));
            latency[i] = zigzag(static_cast<i64>(static_cast<u64>(w.received[i]) - static_cast<u64>(w.occurred[i])));
        }
        e.run_start_.push_back(static_cast<u32>(n));
        e.seq_gap_ = BlockPacked::pack(seq_gap);
        e.occ_delta_ = BlockPacked::pack(occ_delta);
        e.latency_ = BlockPacked::pack(latency);

        e.mode_ = make_col(w.mode, n);
        e.event_name_ = make_col(w.event_name, n);
        e.route_ = make_col(w.route, n);
        e.acquisition_ = make_col(w.acquisition, n);
        e.properties_ = make_col(w.properties, n);

        std::unordered_map<u32, u32> seen;
        std::vector<u32> codes;
        codes.reserve(n);
        for (const u32 v : w.schema_version) {
            const auto [it, inserted] = seen.try_emplace(v, static_cast<u32>(e.schema_dict_.size()));
            if (inserted) e.schema_dict_.push_back(v);
            codes.push_back(it->second);
        }
        e.schema_codes_ = FixedPacked::pack(widen(codes), bits_for_card(e.schema_dict_.size()));
        return e;
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

    [[nodiscard]] RowView row(std::size_t i) const noexcept {
        const auto it = std::upper_bound(run_start_.begin(), run_start_.end(), static_cast<u32>(i));
        const std::size_t r = static_cast<std::size_t>(it - run_start_.begin()) - 1;
        const std::size_t s = run_start_[r];
        u64 seq = static_cast<u64>(unzigzag(seq_gap_.get(s)));
        u64 occ = static_cast<u64>(run_first_occ_[r]);
        for (std::size_t j = s + 1; j <= i; ++j) {
            seq += 1 + static_cast<u64>(unzigzag(seq_gap_.get(j)));
            occ += static_cast<u64>(unzigzag(occ_delta_.get(j)));
        }
        const u64 rec = occ + static_cast<u64>(unzigzag(latency_.get(i)));
        return RowView{event_id_[i],
                       run_session_[r],
                       static_cast<i64>(seq),
                       schema_dict_[schema_codes_.get(i)],
                       static_cast<i64>(occ),
                       static_cast<i64>(rec),
                       text(mode_, i),
                       text(event_name_, i),
                       text(route_, i),
                       text(acquisition_, i),
                       text(properties_, i)};
    }

    [[nodiscard]] u64 s1(std::string_view target) const noexcept {
        const std::size_t code = event_name_.dict.find(target);
        if (code == event_name_.dict.card()) return 0;
        u64 count = 0;
        for (std::size_t i = 0; i < n_; ++i) count += static_cast<u64>(event_name_.codes.get(i) == code);
        return count;
    }
    [[nodiscard]] u64 s2(std::string_view target) const noexcept {
        const std::size_t code = event_name_.dict.find(target);
        if (code == event_name_.dict.card()) return 0;
        u64 acc = 0;
        latency_.for_each_block([&](std::size_t first, const u64* v, std::size_t count) {
            for (std::size_t k = 0; k < count; ++k) {
                if (event_name_.codes.get(first + k) == code) acc += static_cast<u64>(unzigzag(v[k]));
            }
        });
        return acc;
    }
    [[nodiscard]] i64 s3() const noexcept {
        i64 best = std::numeric_limits<i64>::min();
        if (n_ == 0) return best;
        std::size_t r = 0;
        u64 t = 0;
        occ_delta_.for_each_block([&](std::size_t first, const u64* v, std::size_t count) {
            for (std::size_t k = 0; k < count; ++k) {
                const std::size_t i = first + k;
                if (i == run_start_[r + 1]) ++r;
                if (i == run_start_[r]) {
                    t = static_cast<u64>(run_first_occ_[r]);
                } else {
                    t += static_cast<u64>(unzigzag(v[k]));
                }
                best = std::max(best, static_cast<i64>(t));
            }
        });
        return best;
    }

    [[nodiscard]] Breakdown breakdown() const {
        Breakdown b;
        b.add("event_id", event_id_.size() * sizeof(Id128));
        b.add("session_id", run_session_.size() * sizeof(Id128) + run_start_.size() * sizeof(u32));
        b.add("client_seq", seq_gap_.bytes());
        b.add("schema_version", schema_dict_.size() * sizeof(u32) + schema_codes_.bytes());
        b.add("occurred_at", run_first_occ_.size() * sizeof(i64) + occ_delta_.bytes());
        b.add("received_at", latency_.bytes());
        b.add("mode", mode_.dict.footprint() + mode_.codes.bytes());
        b.add("event_name", event_name_.dict.footprint() + event_name_.codes.bytes());
        b.add("route", route_.dict.footprint() + route_.codes.bytes());
        b.add("acquisition", acquisition_.dict.footprint() + acquisition_.codes.bytes());
        b.add("properties", properties_.dict.footprint() + properties_.codes.bytes());
        return b;
    }

private:
    struct Col {
        StringDict dict;
        FixedPacked codes;
    };
    static Col make_col(const StringColumn& c, std::size_t n) {
        DictEncoded e = dictionary_encode(c, n);
        Col col;
        col.codes = FixedPacked::pack(widen(e.codes), bits_for_card(e.dict.card()));
        col.dict = std::move(e.dict);
        return col;
    }
    static std::string_view text(const Col& c, std::size_t i) noexcept { return c.dict.at(c.codes.get(i)); }

    std::size_t n_ = 0;
    std::vector<Id128> event_id_;
    std::vector<Id128> run_session_;
    std::vector<u32> run_start_;  // runs + 1 entries; the last one is n
    std::vector<i64> run_first_occ_;
    BlockPacked seq_gap_;
    BlockPacked occ_delta_;
    BlockPacked latency_;
    std::vector<u32> schema_dict_;
    FixedPacked schema_codes_;
    Col mode_, event_name_, route_, acquisition_, properties_;
};

}  // namespace exp002

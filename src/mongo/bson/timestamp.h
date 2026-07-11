// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_view.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <compare>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <tuple>

namespace mongo {

class BSONObj;

/**
 * Timestamp: A combination of a count of seconds since the POSIX epoch plus an ordinal value.
 */
class [[MONGO_MOD_PUBLIC]] Timestamp {
public:
    // Timestamp to signal that the storage engine should take unstable checkpoints.
    static const Timestamp kAllowUnstableCheckpointsSentinel;

    // Maximum Timestamp value.
    static Timestamp max();

    // Returns the minimum timestamp. Used in the context of selecting and ordering storage engine
    // snapshots.
    static Timestamp min() {
        return Timestamp();
    }

    /**
     * Constructor that builds a Timestamp from a Date_t by using the high-order 4 bytes of "date"
     * for the "secs" field and the low-order 4 bytes for the "i" field.
     */
    explicit Timestamp(Date_t date) : Timestamp(date.toULL()) {}

    /**
     * Constructor that builds a Timestamp from a 64-bit unsigned integer by using
     * the high-order 4 bytes of "v" for the "secs" field and the low-order 4 bytes for the "i"
     * field.
     */
    explicit Timestamp(unsigned long long val) : Timestamp(val >> 32, val) {}

    Timestamp(Seconds s, unsigned increment) : Timestamp(s.count(), increment) {}

    Timestamp(unsigned a, unsigned b) : i(b), secs(a) {}

    Timestamp() = default;

    unsigned getSecs() const {
        return secs;
    }

    unsigned getInc() const {
        return i;
    }

    unsigned long long asULL() const {
        unsigned long long result = secs;
        result <<= 32;
        result |= i;
        return result;
    }
    long long asLL() const {
        return static_cast<long long>(asULL());
    }

    std::int64_t asInt64() const {
        return static_cast<std::int64_t>(asLL());
    }

    bool isNull() const {
        return secs == 0;
    }

    std::string toStringPretty() const;

    std::string toString() const;

    bool operator==(const Timestamp& r) const {
        return tie() == r.tie();
    }
    auto operator<=>(const Timestamp& r) const {
        return tie() <=> r.tie();
    }

    Timestamp operator+(unsigned long long inc) const {
        return Timestamp(asULL() + inc);
    }

    Timestamp operator-(unsigned long long inc) const {
        return Timestamp(asULL() - inc);
    }

    friend std::ostream& operator<<(std::ostream& s, const Timestamp& t) {
        return s << t.toString();
    }

    friend StringBuilder& operator<<(StringBuilder& s, const Timestamp& t) {
        return s << t.toString();
    }

    // Append the BSON representation of this Timestamp to the given BufBuilder with the given
    // name. This lives here because Timestamp manages its own serialization format.

    template <class Builder>
    void append(Builder& builder, std::string_view fieldName) const {
        // No endian conversions needed, since we store in-memory representation
        // in little endian format, regardless of target endian.
        builder.appendNum(static_cast<char>(BSONType::timestamp));
        builder.appendCStr(fieldName);
        builder.appendNum(asULL());
    }

    /// Returns BSON("" << *this)
    BSONObj toBSON() const;

    /**
     * Hash function compatible with absl::Hash for absl::unordered_{map,set}
     */
    template <typename H>
    friend H AbslHashValue(H h, const Timestamp& ts) {
        return H::combine(std::move(h), ts.secs, ts.i);
    }

private:
    std::tuple<unsigned, unsigned> tie() const {
        return std::tie(secs, i);
    }

    unsigned i = 0;
    unsigned secs = 0;
};

}  // namespace mongo

namespace fmt {
template <>
struct formatter<mongo::Timestamp> : formatter<std::string_view> {
    auto format(const mongo::Timestamp& t, auto& ctx) const {
        return fmt::format_to(ctx.out(), "Timestamp({}, {})", t.getSecs(), t.getInc());
    }
};
}  // namespace fmt

/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>
#include <climits>
#include <cstdint>
#include <fmt/format.h>
#include <ostream>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/bufreader.h"

namespace mongo {

/**
 * The key that uniquely identifies a Record in a Collection or RecordStore.
 */
class RecordId {
public:
    // This set of constants define the boundaries of the 'normal' and 'reserved' id ranges.
    static constexpr int64_t kNullRepr = 0;
    static constexpr int64_t kMinRepr = LLONG_MIN;
    static constexpr int64_t kMaxRepr = LLONG_MAX;
    static constexpr int64_t kMinReservedRepr = kMaxRepr - (1024 * 1024);

    /**
     * Enumerates all ids in the reserved range that have been allocated for a specific purpose.
     */
    enum class ReservedId : int64_t { kWildcardMultikeyMetadataId = kMinReservedRepr };

    /**
     * Constructs a Null RecordId.
     */
    RecordId() : _repr(kNullRepr) {}

    explicit RecordId(int64_t repr) : _repr(repr) {}

    explicit RecordId(ReservedId repr) : RecordId(static_cast<int64_t>(repr)) {}

    /**
     * Construct a RecordId from two halves.
     * TODO consider removing.
     */
    RecordId(int high, int low) : _repr((uint64_t(high) << 32) | uint32_t(low)) {}

    /**
     * A RecordId that compares less than all ids that represent documents in a collection.
     */
    static RecordId min() {
        return RecordId(kMinRepr);
    }

    /**
     * A RecordId that compares greater than all ids that represent documents in a collection.
     */
    static RecordId max() {
        return RecordId(kMaxRepr);
    }

    /**
     * Returns the first record in the reserved id range at the top of the RecordId space.
     */
    static RecordId minReserved() {
        return RecordId(kMinReservedRepr);
    }

    bool isNull() const {
        return _repr == 0;
    }

    int64_t repr() const {
        return _repr;
    }

    /**
     * Valid RecordIds are the only ones which may be used to represent Records. The range of valid
     * RecordIds includes both "normal" ids that refer to user data, and "reserved" ids that are
     * used internally. All RecordIds outside of the valid range are sentinel values.
     */
    bool isValid() const {
        return isNormal() || isReserved();
    }

    /**
     * Normal RecordIds are those which fall within the range used to represent normal user data,
     * excluding the reserved range at the top of the RecordId space.
     */
    bool isNormal() const {
        return _repr > 0 && _repr < kMinReservedRepr;
    }

    /**
     * Returns true if this RecordId falls within the reserved range at the top of the record space.
     */
    bool isReserved() const {
        return _repr >= kMinReservedRepr && _repr < kMaxRepr;
    }

    int compare(RecordId rhs) const {
        return _repr == rhs._repr ? 0 : _repr < rhs._repr ? -1 : 1;
    }

    /**
     * Hash value for this RecordId. The hash implementation may be modified, and its behavior
     * may differ across platforms. Hash values should not be persisted.
     */
    struct Hasher {
        size_t operator()(RecordId rid) const {
            size_t hash = 0;
            // TODO consider better hashes
            boost::hash_combine(hash, rid.repr());
            return hash;
        }
    };

    /// members for Sorter
    struct SorterDeserializeSettings {};  // unused
    void serializeForSorter(BufBuilder& buf) const {
        buf.appendNum(static_cast<long long>(_repr));
    }
    static RecordId deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
        return RecordId(buf.read<LittleEndian<int64_t>>());
    }
    int memUsageForSorter() const {
        return sizeof(RecordId);
    }
    RecordId getOwned() const {
        return *this;
    }

    void serialize(fmt::memory_buffer& buffer) const {
        fmt::format_to(buffer, "RecordId({})", _repr);
    }

    void serialize(BSONObjBuilder* builder) const {
        builder->append("RecordId"_sd, _repr);
    }

private:
    int64_t _repr;
};

inline bool operator==(RecordId lhs, RecordId rhs) {
    return lhs.repr() == rhs.repr();
}
inline bool operator!=(RecordId lhs, RecordId rhs) {
    return lhs.repr() != rhs.repr();
}
inline bool operator<(RecordId lhs, RecordId rhs) {
    return lhs.repr() < rhs.repr();
}
inline bool operator<=(RecordId lhs, RecordId rhs) {
    return lhs.repr() <= rhs.repr();
}
inline bool operator>(RecordId lhs, RecordId rhs) {
    return lhs.repr() > rhs.repr();
}
inline bool operator>=(RecordId lhs, RecordId rhs) {
    return lhs.repr() >= rhs.repr();
}

inline StringBuilder& operator<<(StringBuilder& stream, const RecordId& id) {
    return stream << "RecordId(" << id.repr() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const RecordId& id) {
    return stream << "RecordId(" << id.repr() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const boost::optional<RecordId>& id) {
    return stream << "RecordId(" << (id ? id.get().repr() : 0) << ')';
}

}  // namespace mongo

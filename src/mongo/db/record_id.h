/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
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
#include <ostream>

#include "mongo/bson/util/builder.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/util/bufreader.h"

namespace mongo {

/**
 * The key that uniquely identifies a Record in a Collection or RecordStore.
 */
class RecordId {
public:
    /**
     * Constructs a Null RecordId.
     */
    RecordId() : _repr(kNullRepr) {}

    explicit RecordId(int64_t repr) : _repr(repr) {}

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

    bool isNull() const {
        return _repr == 0;
    }

    int64_t repr() const {
        return _repr;
    }

    /**
     * Normal RecordIds are the only ones valid for representing Records. All RecordIds outside
     * of this range are sentinel values.
     */
    bool isNormal() const {
        return _repr > 0 && _repr < kMaxRepr;
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

private:
    static const int64_t kMaxRepr = LLONG_MAX;
    static const int64_t kNullRepr = 0;
    static const int64_t kMinRepr = LLONG_MIN;

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

inline logger::LogstreamBuilder& operator<<(logger::LogstreamBuilder& stream, const RecordId& id) {
    stream.stream() << id;
    return stream;
}
}  // namespace mongo

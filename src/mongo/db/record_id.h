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
#include "mongo/util/hex.h"

namespace mongo {

/**
 * The key that uniquely identifies a Record in a Collection or RecordStore.
 */
class RecordId {
public:
    // This set of constants define the boundaries of the 'normal' id ranges for the int64_t format.
    static constexpr int64_t kMinRepr = LLONG_MIN;
    static constexpr int64_t kMaxRepr = LLONG_MAX;

    // Fixed size of a RecordId that holds a char array
    enum { kSmallStrSize = 12 };

    /**
     * A RecordId that compares less than all int64_t RecordIds that represent documents in a
     * collection.
     */
    static RecordId minLong() {
        return RecordId(kMinRepr);
    }

    /**
     * A RecordId that compares greater than all int64_t RecordIds that represent documents in a
     * collection.
     */
    static RecordId maxLong() {
        return RecordId(kMaxRepr);
    }

    RecordId() : _format(Format::kNull) {}

    /**
     * RecordId supports holding either an int64_t or a 12 byte char array.
     */
    explicit RecordId(int64_t repr) : _storage(repr), _format(Format::kLong) {}
    explicit RecordId(const char* str, int32_t size)
        : _storage(str, size), _format(Format::kSmallStr) {}

    /**
     * Construct a RecordId from two halves.
     */
    RecordId(int high, int low) : RecordId((uint64_t(high) << 32) | uint32_t(low)) {}

    /** Tag for dispatching on null values */
    class Null {};

    /**
     * Helpers to dispatch based on the underlying type.
     */
    template <typename OnNull, typename OnLong, typename OnStr>
    auto withFormat(OnNull&& onNull, OnLong&& onLong, OnStr&& onStr) const {
        switch (_format) {
            case Format::kNull:
                return onNull(Null());
            case Format::kLong:
                return onLong(_storage._long);
            case Format::kSmallStr:
                return onStr(_storage._str, kSmallStrSize);
            default:
                MONGO_UNREACHABLE;
        }
    }

    int64_t asLong() const {
        // In the the int64_t format, null can also be represented by '0'.
        if (_format == Format::kNull) {
            return 0;
        }
        invariant(_format == Format::kLong);
        return _storage._long;
    }

    const char* strData() const {
        invariant(_format == Format::kSmallStr);
        return _storage._str;
    }

    bool isNull() const {
        // In the the int64_t format, null can also represented by '0'.
        if (_format == Format::kLong) {
            return _storage._long == 0;
        }
        return _format == Format::kNull;
    }

    /**
     * Valid RecordIds are the only ones which may be used to represent Records. The range of valid
     * RecordIds includes both "normal" ids that refer to user data, and "reserved" ids that are
     * used internally. All RecordIds outside of the valid range are sentinel values.
     */
    bool isValid() const {
        return withFormat([](Null n) { return false; },
                          [&](int64_t rid) { return rid > 0; },
                          [&](const char* str, int size) { return true; });
    }

    int compare(const RecordId& rhs) const {
        // Null always compares less than every other RecordId.
        if (isNull() && rhs.isNull()) {
            return 0;
        } else if (isNull()) {
            return -1;
        } else if (rhs.isNull()) {
            return 1;
        }
        invariant(_format == rhs._format);
        return withFormat(
            [](Null n) { return 0; },
            [&](const int64_t rid) {
                return rid == rhs._storage._long ? 0 : rid < rhs._storage._long ? -1 : 1;
            },
            [&](const char* str, int size) { return memcmp(str, rhs._storage._str, size); });
    }

    size_t hash() const {
        size_t hash = 0;
        withFormat(
            [](Null n) {},
            [&](int64_t rid) { boost::hash_combine(hash, rid); },
            [&](const char* str, int size) { boost::hash_combine(hash, std::string(str, size)); });
        return hash;
    }

    std::string toString() const {
        return withFormat(
            [](Null n) { return std::string("null"); },
            [](int64_t rid) { return std::to_string(rid); },
            [](const char* str, int size) { return hexblob::encodeLower(str, size); });
    }

    /**
     * Hash value for this RecordId. The hash implementation may be modified, and its behavior
     * may differ across platforms. Hash values should not be persisted.
     */
    struct Hasher {
        size_t operator()(const RecordId& rid) const {
            return rid.hash();
        }
    };

    void serialize(fmt::memory_buffer& buffer) const {
        withFormat([&](Null n) { fmt::format_to(buffer, "RecordId(null)"); },
                   [&](int64_t rid) { fmt::format_to(buffer, "RecordId({})", rid); },
                   [&](const char* str, int size) {
                       fmt::format_to(buffer, "RecordId({})", hexblob::encodeLower(str, size));
                   });
    }

    void serialize(BSONObjBuilder* builder) const {
        withFormat([&](Null n) { builder->append("RecordId", "null"); },
                   [&](int64_t rid) { builder->append("RecordId"_sd, rid); },
                   [&](const char* str, int size) {
                       builder->appendBinData("RecordId"_sd, size, BinDataGeneral, str);
                   });
    }

    /**
     * Enumerates all reserved ids that have been allocated for a specific purpose.
     * The underlying value of the reserved Record ID is data-type specific and must be
     * retrieved by the reservedIdFor() helper.
     */
    enum class Reservation { kWildcardMultikeyMetadataId };

    // These reserved ranges leave 2^20 possible reserved values.
    static constexpr int64_t kMinReservedLong = RecordId::kMaxRepr - (1024 * 1024);
    static constexpr unsigned char kMinReservedOID[OID::kOIDSize] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf0, 0x00, 0x00};

    /**
     * Returns the reserved RecordId value for a given Reservation.
     */
    template <typename T>
    static RecordId reservedIdFor(Reservation res) {
        // There is only one reservation at the moment.
        invariant(res == Reservation::kWildcardMultikeyMetadataId);
        if constexpr (std::is_same_v<T, int64_t>) {
            return RecordId(kMinReservedLong);
        } else {
            static_assert(std::is_same_v<T, OID>, "Unsupported RecordId type");
            OID minReserved(kMinReservedOID);
            return RecordId(minReserved.view().view(), OID::kOIDSize);
        }
    }

    /**
     * Returns true if this RecordId falls within the reserved range for a given RecordId type.
     */
    template <typename T>
    static bool isReserved(RecordId id) {
        if (id.isNull()) {
            return false;
        }
        if constexpr (std::is_same_v<T, int64_t>) {
            return id.asLong() >= kMinReservedLong && id.asLong() < RecordId::kMaxRepr;
        } else {
            static_assert(std::is_same_v<T, OID>, "Unsupported RecordId type");
            return memcmp(id.strData(), kMinReservedOID, OID::kOIDSize) >= 0;
        }
    }

private:
    /**
     * Specifies the storage format of this RecordId.
     */
    enum class Format : uint32_t {
        /** Contains no value */
        kNull,
        /** int64_t */
        kLong,
        /** char[12] */
        kSmallStr
    };

// Pack our union so that it only uses 12 bytes. The union will default to a 8 byte alignment,
// making it 16 bytes total with 4 bytes of padding. Instead, we force the union to use a 4 byte
// alignment, so it packs into 12 bytes. This leaves 4 bytes for our Format, allowing the RecordId
// to use 16 bytes total.
#pragma pack(push, 4)
    union Storage {
        int64_t _long;
        char _str[kSmallStrSize];

        Storage() {}
        Storage(int64_t s) : _long(s) {}
        Storage(const char* str, int32_t size) {
            invariant(size == kSmallStrSize);
            memcpy(_str, str, size);
        }
    };
#pragma pack(pop)

    Storage _storage;
    Format _format;
};


inline bool operator==(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) == 0;
}
inline bool operator!=(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs);
}
inline bool operator<(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) < 0;
}
inline bool operator<=(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) <= 0;
}
inline bool operator>(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) > 0;
}
inline bool operator>=(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) >= 0;
}

inline StringBuilder& operator<<(StringBuilder& stream, const RecordId& id) {
    return stream << "RecordId(" << id.toString() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const RecordId& id) {
    return stream << "RecordId(" << id.toString() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const boost::optional<RecordId>& id) {
    return stream << "RecordId(" << (id ? id->toString() : 0) << ')';
}

}  // namespace mongo

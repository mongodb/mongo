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

    RecordId() {
        _buffer[kBufEnd] = Format::kNull;
    }

    /**
     * Construct a RecordId that holds an int64_t. The raw value for RecordStore storage may be
     * retrieved using getLong().
     */
    explicit RecordId(int64_t s) {
        memcpy(_buffer, &s, sizeof(s));
        _buffer[kBufEnd] = Format::kLong;
    }

    /**
     * Construct a RecordId that holds a small binary string. The raw value for RecordStore storage
     * may be retrieved using getStr().
     */
    explicit RecordId(const char* str, int32_t size) {
        invariant(size > 0, "key size must be greater than 0");
        // Must fit into the 16 byte buffer minus 1 byte for size and 1 format byte.
        uassert(ErrorCodes::BadValue,
                fmt::format("key size {} greater than maximum {}", size, kBufMaxSize - 2),
                size + 2 <= kBufMaxSize);
        _buffer[0] = static_cast<char>(size);
        memcpy(_buffer + 1, str, size);
        _buffer[kBufEnd] = Format::kSmallStr;
    }

    /**
     * Construct a RecordId from two halves.
     */
    RecordId(int high, int low) : RecordId((uint64_t(high) << 32) | uint32_t(low)) {}

    /** Tag for dispatching on null values */
    class Null {};

    /**
     * Helper to dispatch based on the underlying type. In most cases the RecordId type will be
     * known in advance, but this may be used when the type is not known.
     */
    template <typename OnNull, typename OnLong, typename OnStr>
    auto withFormat(OnNull&& onNull, OnLong&& onLong, OnStr&& onStr) const {
        switch (auto f = _format()) {
            case Format::kNull:
                return onNull(Null());
            case Format::kLong:
                return onLong(getLong());
            case Format::kSmallStr: {
                auto str = getStr();
                return onStr(str.rawData(), str.size());
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    // Returns true if this RecordId is storing a long integer.
    bool isLong() const {
        return _format() == Format::kLong;
    }

    // Returns true if this RecordId is storing a binary string.
    bool isStr() const {
        return _format() == Format::kSmallStr;
    }

    /**
     * Returns the raw value to be used as a key in a RecordStore. Requires that this RecordId was
     * constructed with a 64-bit integer value or null; invariants otherwise.
     */
    int64_t getLong() const {
        // In the the int64_t format, null can also be represented by '0'.
        if (_format() == Format::kNull) {
            return 0;
        }
        invariant(isLong());
        int64_t val;
        memcpy(&val, _buffer, sizeof(val));
        return val;
    }

    /**
     * Returns the raw value to be used as a key in a RecordStore. Requires that this RecordId was
     * constructed with a binary string value, and invariants otherwise.
     */
    const StringData getStr() const {
        invariant(isStr());
        char size = _buffer[0];
        invariant(size > 0);
        invariant(size < kBufMaxSize - 1);
        return StringData(_buffer + 1, size);
    }

    /**
     * Returns true if this RecordId is not suitable for storage in a RecordStore.
     */
    bool isNull() const {
        // In the the int64_t format, null can also be represented by '0'.
        if (_format() == Format::kLong) {
            return getLong() == 0;
        }
        return _format() == Format::kNull;
    }


    /**
     * Valid RecordIds are the only ones which may be used to represent Records. The range of valid
     * RecordIds includes both "normal" ids that refer to user data, and "reserved" ids that are
     * used internally. All RecordIds outside of the valid range are sentinel values.
     */
    bool isValid() const {
        return withFormat(
            [](Null n) { return false; },
            [&](int64_t rid) { return rid > 0; },
            [&](const char* str, int size) { return size > 0 && size + 2 <= kBufMaxSize; });
    }

    /**
     * Compares two RecordIds. Requires that both RecordIds are of the same format, unless one or
     * both are null. Null always compares less than every other RecordId format.
     */
    int compare(const RecordId& rhs) const {
        if (_format() == Format::kNull && rhs._format() == Format::kNull) {
            return 0;
        } else if (_format() == Format::kNull) {
            return -1;
        } else if (rhs._format() == Format::kNull) {
            return 1;
        }
        invariant(_format() == rhs._format());
        return withFormat(
            [](Null n) { return 0; },
            [&](const int64_t rid) {
                return rid == rhs.getLong() ? 0 : (rid > rhs.getLong()) ? 1 : -1;
            },
            [&](const char* str, int size) { return StringData(str, size).compare(rhs.getStr()); });
    }

    size_t hash() const {
        size_t hash = 0;
        withFormat([](Null n) {},
                   [&](int64_t rid) { boost::hash_combine(hash, rid); },
                   [&](const char* str, int size) {
                       boost::hash_combine(hash, std::string_view(str, size));
                   });
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

    /**
     * Formats this RecordId into a human-readable BSON object that may be passed around and
     * deserialized with deserializeToken().
     * Note: This is not to be used as a key to a RecordStore.
     */
    void serializeToken(StringData fieldName, BSONObjBuilder* builder) const {
        // Preserve the underlying format by using a different BSON type for each format.
        withFormat([&](Null n) { builder->appendNull(fieldName); },
                   [&](int64_t rid) { builder->append(fieldName, rid); },
                   [&](const char* str, int size) {
                       builder->append(fieldName, hexblob::encodeLower(str, size));
                   });
    }

    /**
     * Decode a token created by serializeToken().
     */
    static RecordId deserializeToken(const BSONElement& elem) {
        if (elem.isNull()) {
            return RecordId();
        } else if (elem.isNumber()) {
            return RecordId(elem.numberLong());
        } else if (elem.type() == BSONType::String) {
            auto str = hexblob::decode(elem.String());
            return RecordId(str.c_str(), str.size());
        } else {
            uasserted(ErrorCodes::BadValue,
                      fmt::format("Could not deserialize RecordId with type {}", elem.type()));
        }
    }

private:
    enum { kBufEnd = 15, kBufMaxSize = 16 };

    /**
     * Specifies the storage format of this RecordId.
     */
    enum Format : int8_t {
        /** Contains no value */
        kNull,
        /** int64_t */
        kLong,
        /** variable-length binary string, up to 14 bytes */
        kSmallStr
    };

    Format _format() const {
        return static_cast<Format>(_buffer[kBufEnd]);
    }

    // Storage for this RecordId.
    // - The last byte stores the Format.
    // - For the kLong type, the first 8 bytes encode the value in machine-endian order.
    // - For the kSmallStr type, the first byte encodes the length and the remaining bytes encode
    // the string.
    char _buffer[kBufMaxSize];
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

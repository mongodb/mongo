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

#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"
#include <algorithm>
#include <array>
#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <ostream>

#include "mongo/bson/util/builder.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/hex.h"
#include <string_view>

namespace mongo {
enum class Format : int8_t {
    /* Uninitialized and contains no value */
    kNull,
    /**
     * Stores an integer. The first 8 bytes of '_buffer' encode the value in machine-endian
     * order. The RecordId may only be accessed using getLong().
     */
    kLong,
    /**
     * Stores a variable-length binary string smaller than kSmallStrMaxSize. The first byte of
     * '_buffer' encodes the length and the remaining bytes store the string. This RecordId may
     * only be accessed using getStr().
     */
    kSmallStr,
    /**
     * Stores a variable-length binary string larger than kSmallStrMaxSize. The value is stored
     * in a reference-counted buffer, '_sharedBuffer'. This RecordId may only be accessed using
     * getStr().
     */
    kBigStr
};

/**
 * The key that uniquely identifies a Record in a Collection or RecordStore.
 */
class RecordId {
public:
    static constexpr int64_t kMaxRepr = LLONG_MAX;
    static constexpr int64_t kNullRepr = 0;
    static constexpr int64_t kMinRepr = LLONG_MIN;
    static constexpr int64_t kSmallStrMaxSize = 22;
    static constexpr int64_t kBigStrMaxSize = 8 * 1024 * 1024;
    /**
     * Constructs a Null RecordId.
     */
    RecordId() = default;

    explicit RecordId(int64_t repr) {
        _format = Format::kLong;
        const char* valPtr = reinterpret_cast<const char*>(&repr);
        std::copy(valPtr, valPtr + sizeof(repr), _buffer.begin());
    }

    explicit RecordId(const char* data, size_t size) {
        invariant(size > 0, "key size must be greater than 0");
        if (size <= kSmallStrMaxSize) {
            _format = Format::kSmallStr;
            // Must fit into the buffer minus 1 byte for size.
            _buffer[0] = static_cast<char>(size);
            // memcpy(_buffer.data() + 1, data, size);
            std::copy(data, data + size, &_buffer[1]);

        } else if (size <= kBigStrMaxSize) {
            _format = Format::kBigStr;
            auto sharedBuf = SharedBuffer::allocate(size);
            std::copy(data, data + size, sharedBuf.get());
            _sharedBuffer = std::move(sharedBuf);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    /**
     * Construct a RecordId from two halves.
     * TODO consider removing.
     */
    RecordId(int high, int low) : RecordId((uint64_t(high) << 32) | uint32_t(low)) {}

    void setValue(int64_t val) {
        _format = Format::kLong;
        const char* valPtr = reinterpret_cast<const char*>(&val);
        std::copy(valPtr, valPtr + sizeof(val), _buffer.begin());
    }

    void setValue(const char* data, size_t size) {
        invariant(size > 0, "key size must be greater than 0");
        if (size <= kSmallStrMaxSize) {
            _format = Format::kSmallStr;
            // Must fit into the buffer minus 1 byte for size.
            _buffer[0] = static_cast<char>(size);
            // memcpy(_buffer.data() + 1, data, size);
            std::copy(data, data + size, &_buffer[1]);

        } else if (size <= kBigStrMaxSize) {
            _format = Format::kBigStr;
            auto sharedBuf = SharedBuffer::allocate(size);
            std::copy(data, data + size, sharedBuf.get());
            _sharedBuffer = std::move(sharedBuf);
        } else {
            MONGO_UNREACHABLE;
        }
    }

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

    /** Tag for dispatching on null values */
    class Null {};
    /**
     * Helper to dispatch based on the underlying type. In most cases the RecordId type will be
     * known in advance, but this may be used when the type is not known.
     */
    template <typename OnNull, typename OnLong, typename OnStr>
    auto withFormat(OnNull&& onNull, OnLong&& onLong, OnStr&& onStr) const {
        switch (_format) {
            case Format::kNull:
                return onNull(Null());
            case Format::kLong:
                return onLong(_getLongNoCheck());
            case Format::kSmallStr: {
                auto str = _getSmallStrNoCheck();
                return onStr(str.data(), str.size());
            }
            case Format::kBigStr: {
                auto str = _getBigStrNoCheck();
                return onStr(str.data(), str.size());
            }
            default:
                MONGO_UNREACHABLE;
        }
    }


    std::string toString() const {

        return withFormat([](Null n) { return std::string("null"); },
                          [](int64_t rid) { return std::to_string(rid); },
                          [](const char* str, int size) { return std::string(str, size); });
    }

    bool isNull() const {
        if (_format == Format::kLong) {
            return repr() == kNullRepr;
        }
        return _format == Format::kNull;
    }

    bool isLong() const {
        return _format == Format::kLong;
    }

    bool isStr() const {
        return _format == Format::kSmallStr || _format == Format::kBigStr;
    }

    // repr() now just for Format::kLong
    int64_t repr() const {
        if (_format == Format::kNull) {
            return 0;
        }
        invariant(isLong());
        return _getLongNoCheck();
    }

    /**
     * Returns the raw value to be used as a key in a RecordStore. Requires that this RecordId was
     * constructed with a 64-bit integer value or null; invariants otherwise.
     */
    int64_t getLong() const {
        // In the the int64_t format, null can also be represented by '0'.
        if (_format == Format::kNull) {
            return 0;
        }
        invariant(isLong());
        return _getLongNoCheck();
    }

    /**
     * Returns the raw value to be used as a key in a RecordStore. Requires that this RecordId was
     * constructed with a binary string value, and invariants otherwise.
     */
    const std::string_view getStr() const {
        invariant(isStr());
        if (_format == Format::kSmallStr) {
            return _getSmallStrNoCheck();
        } else if (_format == Format::kBigStr) {
            return _getBigStrNoCheck();
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Normal RecordIds are the only ones valid for representing Records. All RecordIds outside
     * of this range are sentinel values.
     */
    bool isNormal() const {
        if (_format == Format::kNull) {
            return false;
        } else {
            return true;
        }
    }

    int compare(RecordId rhs) const {
        switch (_format) {
            case Format::kNull:
                return rhs._format == Format::kNull ? 0 : -1;
            case Format::kLong:
                if (rhs._format == Format::kNull) {
                    return 1;
                }
                return _getLongNoCheck() == rhs.getLong() ? 0
                    : (_getLongNoCheck() > rhs.getLong()) ? 1
                                                          : -1;
            case Format::kSmallStr:
                if (rhs._format == Format::kNull) {
                    return 1;
                }
                return _getSmallStrNoCheck().compare(rhs.getStr());
            case Format::kBigStr:
                if (rhs._format == Format::kNull) {
                    return 1;
                }
                return _getBigStrNoCheck().compare(rhs.getStr());
        }
        MONGO_UNREACHABLE;
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
        invariant(isLong());
        buf.appendNum(static_cast<long long>(_getLongNoCheck()));
    }
    static RecordId deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
        return RecordId(buf.read<LittleEndian<int64_t>>());
    }
    int memUsageForSorter() const {
        return sizeof(RecordId) + _sharedBuffer.capacity();
        ;
    }
    RecordId getOwned() const {
        return *this;
    }

private:
    int64_t _getLongNoCheck() const {
        int64_t val;
        std::copy(
            _buffer.begin(), _buffer.begin() + sizeof(int64_t), reinterpret_cast<char*>(&val));
        return val;
    }

    std::string_view _getSmallStrNoCheck() const {
        uint8_t size = static_cast<uint8_t>(_buffer[0]);
        invariant(size > 0);
        invariant(size <= kSmallStrMaxSize);
        return std::string_view(_buffer.data() + sizeof(char), size);
    }

    std::string_view _getBigStrNoCheck() const {
        size_t size = _sharedBuffer.capacity();
        invariant(size > kSmallStrMaxSize);
        invariant(size <= kBigStrMaxSize);
        return std::string_view(_sharedBuffer.get(), size);
    }


    // int64_t _repr;
    Format _format = Format::kNull;
    std::array<char, kSmallStrMaxSize + 1> _buffer;
    ConstSharedBuffer _sharedBuffer;
};

inline bool operator==(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) == 0;
    // return lhs.repr() == rhs.repr();
}
inline bool operator!=(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs);
    // return lhs.repr() != rhs.repr();
}
inline bool operator<(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) < 0;
    // return lhs.repr() < rhs.repr();
}
inline bool operator<=(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) <= 0;
    // return lhs.repr() <= rhs.repr();
}
inline bool operator>(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) > 0;
    // return lhs.repr() > rhs.repr();
}
inline bool operator>=(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) >= 0;
    // return lhs.repr() >= rhs.repr();
}

inline StringBuilder& operator<<(StringBuilder& stream, const RecordId& id) {
    return stream << "RecordId(" << id.toString() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const RecordId& id) {
    return stream << "RecordId(" << id.toString() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const boost::optional<RecordId>& id) {
    return stream << "RecordId(" << (id ? id.get().toString() : "null") << ')';
}

inline logger::LogstreamBuilder& operator<<(logger::LogstreamBuilder& stream, const RecordId& id) {
    stream.stream() << id;
    return stream;
}
}  // namespace mongo

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

#include <array>
#include <boost/container_hash/extensions.hpp>
#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>
#include <climits>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <new>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/hex.h"
#include "mongo/util/shared_buffer.h"

namespace mongo {

namespace details {
class RecordIdChecks;
}

/**
 * The key that uniquely identifies a Record in a Collection or RecordStore.
 */
class alignas(int64_t) RecordId {
    // The alignas is necessary in order to comply with memory alignment. Internally we're using
    // 8-byte aligned data members (int64_t / ConstSharedBuffer) but as we're packing the structure
    // the compiler will set the alignment to 1 so we must correct its alignment information for
    // users of the class.

    // Class used for static assertions that can only happen when RecordId is completely defined.
    friend class details::RecordIdChecks;

public:
    // This set of constants define the boundaries of the 'normal' id ranges for the int64_t format.
    static constexpr int64_t kMinRepr = std::numeric_limits<int64_t>::min();
    static constexpr int64_t kMaxRepr = std::numeric_limits<int64_t>::max();

    // A RecordId binary string cannot be larger than this arbitrary size. RecordIds get written to
    // the key and the value in WiredTiger, so we should avoid large strings.
    static constexpr int64_t kBigStrMaxSize = 8 * 1024 * 1024;

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

    ~RecordId() {
        if (_format == Format::kBigStr) {
            HeapStr::getBufferFrom(_data).~ConstSharedBuffer();
        }
    }

    RecordId(RecordId&& other) : _format(other._format), _data(other._data) {
        other._format = kNull;
    }

    RecordId(const RecordId& other) : _format(other._format), _data(other._data) {
        if (_format == Format::kBigStr) {
            // Re-initialize the SharedBuffer to get the correct reference count.
            auto* buffer = &HeapStr::getBufferFrom(_data);
            new (buffer) ConstSharedBuffer(HeapStr::getBufferFrom(other._data));
        }
    };

    RecordId& operator=(const RecordId& other) {
        RecordId tmp{other};
        *this = std::move(tmp);
        return *this;
    }


    RecordId& operator=(RecordId&& other) {
        if (_format == Format::kBigStr) {
            HeapStr::getBufferFrom(_data).~ConstSharedBuffer();
        }
        std::memcpy(reinterpret_cast<void*>(this), &other, sizeof(RecordId));
        other._format = kNull;
        return *this;
    }

    /**
     * Construct a RecordId that holds an int64_t. The raw value for RecordStore storage may be
     * retrieved using getLong().
     */
    explicit RecordId(int64_t s) {
        _format = Format::kLong;
        LongId::getIdFrom(_data) = s;
    }

    /**
     * Construct a RecordId that holds a binary string. The raw value for RecordStore storage may be
     * retrieved using getStr().
     */
    explicit RecordId(const char* str, int32_t size) {
        uassert(8273007, fmt::format("key size must be greater than 0. size: {}", size), size > 0);
        uassert(
            5894900,
            fmt::format("Size of RecordId ({}) is above limit of {} bytes", size, kBigStrMaxSize),
            size <= kBigStrMaxSize);
        if (size <= kSmallStrMaxSize) {
            _format = Format::kSmallStr;
            InlineStr::getSizeFrom(_data) = static_cast<uint8_t>(size);
            auto& arr = InlineStr::getArrayFrom(_data);
            std::memcpy(arr.data(), str, size);
        } else {
            _format = Format::kBigStr;
            auto buffer = SharedBuffer::allocate(size);
            std::memcpy(buffer.get(), str, size);
            auto* bufferPtr = &HeapStr::getBufferFrom(_data);
            new (bufferPtr) ConstSharedBuffer(std::move(buffer));
        }
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
        switch (_format) {
            case Format::kNull:
                return onNull(Null());
            case Format::kLong:
                return onLong(_getLongNoCheck());
            case Format::kSmallStr: {
                auto str = _getSmallStrNoCheck();
                return onStr(str.rawData(), str.size());
            }
            case Format::kBigStr: {
                auto str = _getBigStrNoCheck();
                return onStr(str.rawData(), str.size());
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    // Returns true if this RecordId is storing a long integer.
    bool isLong() const {
        return _format == Format::kLong;
    }

    // Returns true if this RecordId is storing a binary string.
    bool isStr() const {
        return _format == Format::kSmallStr || _format == Format::kBigStr;
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
        dassert(isLong(),
                fmt::format("expected RecordID long format, got: {}", _formatToString(_format)));
        return _getLongNoCheck();
    }

    /**
     * Returns the raw value to be used as a key in a RecordStore. Requires that this RecordId was
     * constructed with a binary string value, and invariants otherwise.
     */
    StringData getStr() const {
        dassert(isStr(),
                fmt::format("expected RecordID string format, got: {}", _formatToString(_format)));
        if (_format == Format::kSmallStr) {
            return _getSmallStrNoCheck();
        } else {
            return _getBigStrNoCheck();
        }
    }

    /**
     * Returns true if this RecordId is not suitable for storage in a RecordStore.
     */
    bool isNull() const {
        switch (_format) {
            case Format::kNull:
                return true;
            case Format::kLong:
                // In the the int64_t format, null can also be represented by '0'.
                return getLong() == 0;
            default:
                return false;
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Returns whether the data for the RecordId is completely stored inline (within the class
     * memory allocation). The only cases where this won't be true is when the RecordId contains a
     * large key string that cannot be allocated inline completely.
     */
    bool isInlineAllocated_forTest() {
        return _format != Format::kBigStr;
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
            [&](const char* str, int size) { return size > 0 && size <= kBigStrMaxSize; });
    }

    /**
     * Compares two RecordIds. Requires that both RecordIds are of the same "type" (long or string).
     * Null is always comparable and is less than every other RecordId format.
     */
    int compare(const RecordId& rhs) const {
        switch (_format) {
            case Format::kNull:
                return rhs._format == Format::kNull ? 0 : -1;
            case Format::kLong: {
                if (rhs._format == Format::kNull) {
                    return 1;
                }
                auto ourId = _getLongNoCheck();
                auto rhsId = rhs.getLong();
                return ourId == rhsId ? 0 : (ourId > rhsId) ? 1 : -1;
            }
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

    std::strong_ordering operator<=>(const RecordId& rhs) const {
        return compare(rhs) <=> 0;
    }

    bool operator==(const RecordId& rhs) const {
        return std::is_eq(*this <=> rhs);
    }

    bool operator!=(const RecordId& rhs) const = default;

    size_t hash() const {
        size_t hash = 0;
        withFormat([](Null n) {},
                   [&](int64_t rid) { boost::hash_combine(hash, rid); },
                   [&](const char* str, int size) {
                       boost::hash_combine(hash, std::string_view(str, size));
                   });
        return hash;
    }

    /**
     * Returns a string form of this RecordId, but in the cases where RecordId is either
     * Format::kSmallStr or Format::kBigStr this is a raw hex dump. For a human-readable string call
     * toStringHumanReadable().
     */
    std::string toString() const {
        return withFormat(
            [](Null n) { return std::string("null"); },
            [](int64_t rid) { return std::to_string(rid); },
            [](const char* str, int size) { return hexblob::encodeLower(str, size); });
    }

    /**
     * Returns a human-readable string form of this RecordId. (Call toString() if you prefer a hex
     * dump of string IDs.)
     */
    std::string toStringHumanReadable() const {
        switch (_format) {
            case Format::kNull:
                return "null";
            case Format::kLong:
                return std::to_string(_getLongNoCheck());
            case Format::kSmallStr: {
                StringData str = _getSmallStrNoCheck();
                return "kSmallStr size: " + std::to_string(str.size()) + " string: '" +
                    std::string(str.rawData()) + "'";
            }
            case Format::kBigStr: {
                StringData str = _getBigStrNoCheck();
                return "kBigStr size: " + std::to_string(str.size()) + " string: '" +
                    std::string(str.rawData()) + "'";
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    /**
     * Returns the total amount of memory used by this RecordId, including itself and any heap
     * buffers.
     */
    size_t memUsage() const {
        size_t largeStrSize =
            (_format == Format::kBigStr) ? HeapStr::getBufferFrom(_data).capacity() : 0;
        return sizeof(RecordId) + largeStrSize;
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
                       builder->appendBinData(fieldName, size, BinDataGeneral, str);
                   });
    }

    /**
     * Same as above but in a binary.
     */
    void serializeToken(BufBuilder& buf) const {
        buf.appendChar(static_cast<char>(_format));
        withFormat([&](Null) {},
                   [&](int64_t rid) { buf.appendNum(rid); },
                   [&](const char* str, int size) {
                       buf.appendNum(size);
                       buf.appendBuf(str, size);
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
        } else if (elem.type() == BSONType::BinData) {
            int size;
            auto str = elem.binData(size);
            return RecordId(str, size);
        } else {
            uasserted(ErrorCodes::BadValue,
                      fmt::format("Could not deserialize RecordId with type {}", elem.type()));
        }
    }

    /**
     * Decode a token created by serializeToken().
     */
    static RecordId deserializeToken(BufReader& buf) {
        auto format = static_cast<Format>(buf.read<char>());
        if (format == Format::kNull) {
            return RecordId();
        } else if (format == Format::kLong) {
            return RecordId(buf.read<LittleEndian<int64_t>>());
        } else if (format == Format::kSmallStr || format == Format::kBigStr) {
            const int size = buf.read<LittleEndian<int>>();
            const char* str = static_cast<const char*>(buf.skip(size));
            return RecordId(str, size);
        } else {
            uasserted(ErrorCodes::BadValue,
                      fmt::format("Could not deserialize RecordId with type {}",
                                  static_cast<int8_t>(format)));
        }
    }

    /**
     * This maximum size for 'small' strings was chosen as a good tradeoff between keeping the
     * RecordId struct lightweight to copy by value (32 bytes), but also making the struct large
     * enough to hold a wider variety of strings. Larger strings must be stored in the
     * heap, which requires an extra memory allocation and makes it more expensive to copy.
     */
    static constexpr auto kSmallStrMaxSize = 30;

    /// members for Sorter
    struct SorterDeserializeSettings {};  // unused
    void serializeForSorter(BufBuilder& buf) const {
        serializeToken(buf);
    }
    static RecordId deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
        return deserializeToken(buf);
    }
    int memUsageForSorter() const {
        return memUsage();
    }
    RecordId getOwned() const {
        MONGO_UNREACHABLE;
    }
    void makeOwned() {
        MONGO_UNREACHABLE;
    }

private:
    /**
     * Format specifies the in-memory representation of this RecordId. This does not represent any
     * durable storage format.
     */
    enum Format : uint8_t {
        /* Uninitialized and contains no value */
        kNull,
        /**
         * Stores an integer. Data is stored in '_data.longId.id'. The RecordId may only be accessed
         * using getLong().
         */
        kLong,
        /**
         * Stores a variable-length binary string less than or equal to kSmallStrMaxSize. Data is
         * stored in the InlineStr struct at '_data.inlineStr'. This RecordId may only be accessed
         * using getStr().
         */
        kSmallStr,
        /**
         * Stores a variable-length binary string larger than kSmallStrMaxSize. The value is stored
         * in a SharedBuffer '_data.heapStr.buffer'.
         * This RecordId may only be accessed using getStr().
         */
        kBigStr
    };

    static std::string _formatToString(Format f) {
        switch (f) {
            case Format::kNull:
                return "null";
            case Format::kLong:
                return "long";
            case Format::kSmallStr:
                return "smallStr";
            case Format::kBigStr:
                return "bigStr";
        }
        MONGO_UNREACHABLE;
    }

    int64_t _getLongNoCheck() const {
        return LongId::getIdFrom(_data);
    }

    StringData _getSmallStrNoCheck() const {
        return StringData(InlineStr::getArrayFrom(_data).data(), InlineStr::getSizeFrom(_data));
    }

    StringData _getBigStrNoCheck() const {
        const auto& buffer = HeapStr::getBufferFrom(_data);
        return StringData(buffer.get(), buffer.capacity());
    }

    static constexpr auto kTargetSizeInBytes = 32;
    // In the usual case we would store the data as Format followed by a struct union of the
    // InlineString (size + array), HeapStr (ConstSharedBuffer), and LongId (int64_t). This however
    // leaves 7 bytes unused for pading if Format is 1 byte and 4 if it is 4 bytes (x86) due to data
    // alignment requirements of the union. To avoid this we manually perform memory padding in the
    // structs of the union coupled with packing the class so that all items align properly.
    Format _format;  // offset = 0, size = 1
    static_assert(sizeof(Format) == 1);
    // All of this will work if and only if char size is 1 (std::byte) for the InlineString.
    static_assert(sizeof(std::byte) == sizeof(char));
    using Content = std::array<std::byte, kTargetSizeInBytes - sizeof(Format)>;
    Content _data;
    // Offsets/padding will be computed in respect to the whole class by taking into account the
    // Format data member.
    struct HeapStr {
        static_assert(std::alignment_of_v<ConstSharedBuffer> <= 8,
                      "ConstSharedBuffer is expected to have 8 bytes alignment at most. Having a "
                      "larger alignment requires changing the RecordId class alignment");
        static constexpr auto kOffset = std::alignment_of_v<ConstSharedBuffer> - sizeof(Format);
        ConstSharedBuffer buffer;  // offset = 1 + 7, size = 8
        static ConstSharedBuffer& getBufferFrom(Content& data) {
            ConstSharedBuffer* ptr = (ConstSharedBuffer*)(data.data() + kOffset);
            return *ptr;
        }
        static const ConstSharedBuffer& getBufferFrom(const Content& data) {
            ConstSharedBuffer* ptr = (ConstSharedBuffer*)(data.data() + kOffset);
            return *ptr;
        }
    };
    struct InlineStr {
        static constexpr auto kSizeOffset = 0;
        static uint8_t& getSizeFrom(Content& data) {
            uint8_t* ptr = (uint8_t*)(data.data() + kSizeOffset);
            return *ptr;
        };
        static const uint8_t& getSizeFrom(const Content& data) {
            uint8_t* ptr = (uint8_t*)(data.data() + kSizeOffset);
            return *ptr;
        };
        static constexpr auto kArrayOffset = sizeof(uint8_t);
        using arr = std::array<char, kTargetSizeInBytes - sizeof(Format) - sizeof(uint8_t)>;
        static arr& getArrayFrom(Content& data) {
            arr* ptr = (arr*)(data.data() + kArrayOffset);
            return *ptr;
        };
        static const arr& getArrayFrom(const Content& data) {
            arr* ptr = (arr*)(data.data() + kArrayOffset);
            return *ptr;
        };
    };
    struct LongId {
        static constexpr auto kOffset = std::alignment_of_v<int64_t> - sizeof(Format);
        static int64_t& getIdFrom(Content& data) {
            int64_t* result = (int64_t*)(data.data() + kOffset);
            return *result;
        }
        static const int64_t& getIdFrom(const Content& data) {
            int64_t* result = (int64_t*)(data.data() + kOffset);
            return *result;
        }
    };
};

namespace details {
// Various assertions of RecordId that can only happen when the type is completely defined.
class RecordIdChecks {
    static_assert(sizeof(RecordId) == RecordId::kTargetSizeInBytes);
    static_assert(std::alignment_of_v<RecordId> == std::alignment_of_v<int64_t>);
};
}  // namespace details

inline StringBuilder& operator<<(StringBuilder& stream, const RecordId& id) {
    return stream << "RecordId(" << id.toString() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const RecordId& id) {
    return stream << "RecordId(" << id.toString() << ')';
}

}  // namespace mongo

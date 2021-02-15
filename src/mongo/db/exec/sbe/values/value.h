/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <array>
#include <bitset>
#include <cstdint>
#include <ostream>
#include <pcre.h>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/represent_as.h"

namespace mongo {
/**
 * Forward declaration.
 */
namespace KeyString {
class Value;
}

class TimeZoneDatabase;

class JsFunction;

namespace sbe {
using FrameId = int64_t;
using SpoolId = int64_t;

using IndexKeysInclusionSet = std::bitset<Ordering::kMaxCompoundIndexKeys>;

namespace value {

static constexpr std::int32_t kStringMaxDisplayLength = 160;
static constexpr std::int32_t kBinDataMaxDisplayLength = 80;
static constexpr std::int32_t kNewUUIDLength = 16;

/**
 * Type dispatch tags.
 */
enum class TypeTags : uint8_t {
    // The value does not exist, aka Nothing in the Maybe monad.
    Nothing = 0,

    // Numerical data types.
    NumberInt32,
    NumberInt64,
    NumberDouble,
    NumberDecimal,

    // Date data types.
    Date,
    Timestamp,

    Boolean,
    Null,
    StringSmall,
    StringBig,
    Array,
    ArraySet,
    Object,

    ObjectId,
    RecordId,

    MinKey,
    MaxKey,

    // Raw bson values.
    bsonObject,
    bsonArray,
    bsonString,
    bsonObjectId,
    bsonBinData,
    // The bson prefix signifies the fact that this type can only come from BSON (either from disk
    // or from user over the wire). It is never created or manipulated by SBE.
    bsonUndefined,
    bsonRegex,
    bsonJavascript,

    // KeyString::Value
    ksValue,

    // Pointer to a compiled PCRE regular expression object.
    pcreRegex,

    // Pointer to a timezone database object.
    timeZoneDB,

    // Pointer to a compiled JS function with scope.
    jsFunction,

    // Pointer to a ShardFilterer for shard filtering.
    shardFilterer,

    // Pointer to a collator interface object.
    collator,
};

inline constexpr bool isNumber(TypeTags tag) noexcept {
    return tag == TypeTags::NumberInt32 || tag == TypeTags::NumberInt64 ||
        tag == TypeTags::NumberDouble || tag == TypeTags::NumberDecimal;
}

inline constexpr bool isString(TypeTags tag) noexcept {
    return tag == TypeTags::StringSmall || tag == TypeTags::StringBig ||
        tag == TypeTags::bsonString;
}

inline constexpr bool isObject(TypeTags tag) noexcept {
    return tag == TypeTags::Object || tag == TypeTags::bsonObject;
}

inline constexpr bool isArray(TypeTags tag) noexcept {
    return tag == TypeTags::Array || tag == TypeTags::ArraySet || tag == TypeTags::bsonArray;
}

inline constexpr bool isObjectId(TypeTags tag) noexcept {
    return tag == TypeTags::ObjectId || tag == TypeTags::bsonObjectId;
}

inline constexpr bool isBinData(TypeTags tag) noexcept {
    return tag == TypeTags::bsonBinData;
}

inline constexpr bool isRecordId(TypeTags tag) noexcept {
    return tag == TypeTags::RecordId;
}

inline constexpr bool isPcreRegex(TypeTags tag) noexcept {
    return tag == TypeTags::pcreRegex;
}

inline constexpr bool isCollatableType(TypeTags tag) noexcept {
    return isString(tag) || isArray(tag) || isObject(tag);
}

inline constexpr bool isBsonRegex(TypeTags tag) noexcept {
    return tag == TypeTags::bsonRegex;
}

BSONType tagToType(TypeTags tag) noexcept;

/**
 * This function takes an SBE TypeTag, looks up the corresponding BSONType t, and then returns a
 * bitmask representation of a set of BSONTypes that contains only BSONType t.
 *
 * For details on how sets of BSONTypes are represented as bitmasks, see mongo::getBSONTypeMask().
 */
inline uint32_t getBSONTypeMask(value::TypeTags tag) noexcept {
    BSONType t = value::tagToType(tag);
    return getBSONTypeMask(t);
}

/**
 * The runtime value. It is a simple 64 bit integer.
 */
using Value = uint64_t;

/**
 * Sort direction of ordered sequence.
 */
enum class SortDirection : uint8_t { Descending, Ascending };

/**
 * Forward declarations.
 */
void releaseValue(TypeTags tag, Value val) noexcept;
std::pair<TypeTags, Value> copyValue(TypeTags tag, Value val);
std::size_t hashValue(TypeTags tag,
                      Value val,
                      const CollatorInterface* collator = nullptr) noexcept;

/**
 * Overloads for writing values and tags to stream.
 */
std::ostream& operator<<(std::ostream& os, const TypeTags tag);
str::stream& operator<<(str::stream& str, const TypeTags tag);
std::ostream& operator<<(std::ostream& os, const std::pair<TypeTags, Value>& value);
str::stream& operator<<(str::stream& str, const std::pair<TypeTags, Value>& value);

/**
 * Three ways value comparison (aka spaceship operator).
 */
std::pair<TypeTags, Value> compareValue(
    TypeTags lhsTag,
    Value lhsValue,
    TypeTags rhsTag,
    Value rhsValue,
    const StringData::ComparatorInterface* comparator = nullptr);

bool isNaN(TypeTags tag, Value val);

/**
 * A simple hash combination.
 */
inline std::size_t hashInit() noexcept {
    return 17;
}

inline std::size_t hashCombine(std::size_t state, std::size_t val) noexcept {
    return state * 31 + val;
}

/**
 * RAII guard.
 */
class ValueGuard {
public:
    ValueGuard(const std::pair<TypeTags, Value> typedValue)
        : ValueGuard(typedValue.first, typedValue.second) {}
    ValueGuard(TypeTags tag, Value val) : _tag(tag), _value(val) {}
    ValueGuard() = delete;
    ValueGuard(const ValueGuard&) = delete;
    ValueGuard(ValueGuard&& other) = delete;
    ~ValueGuard() {
        releaseValue(_tag, _value);
    }

    ValueGuard& operator=(const ValueGuard&) = delete;
    ValueGuard& operator=(ValueGuard&& other) = delete;

    void reset() {
        _tag = TypeTags::Nothing;
        _value = 0;
    }

private:
    TypeTags _tag;
    Value _value;
};

inline char* getRawPointerView(Value val) noexcept {
    return reinterpret_cast<char*>(val);
}

inline Decimal128 readDecimal128FromMemory(const ConstDataView& view) {
    uint64_t low = view.read<LittleEndian<uint64_t>>();
    uint64_t high = view.read<LittleEndian<uint64_t>>(sizeof(uint64_t));
    return Decimal128{Decimal128::Value{low, high}};
}

template <class T>
struct dont_deduce_t {
    using type = T;
};

template <class T>
using dont_deduce = typename dont_deduce_t<T>::type;

template <typename T>
Value bitcastFrom(const dont_deduce<T> in) noexcept {
    static_assert(std::is_pointer_v<T> || std::is_integral_v<T> || std::is_floating_point_v<T>);

    static_assert(sizeof(Value) >= sizeof(T));

    // Callers must not try to store a pointer to a Decimal128 object in an sbe::value::Value. Any
    // Value with the NumberDecimal TypeTag actually stores a pointer to a NumberDecimal as it would
    // be represented in a BSONElement: a pair of network-ordered (little-endian) uint64_t values.
    // These bytes are _not_ guaranteed to be the same as the bytes in a Decimal128_t object.
    //
    // To get a NumberDecimal value, either call makeCopyDecimal() or store the value in BSON and
    // use sbe::bson::convertFrom().
    static_assert(!std::is_same_v<Decimal128, T>);
    static_assert(!std::is_same_v<Decimal128*, T>);

    if constexpr (std::is_pointer_v<T>) {
        // Casting from pointer to integer value is OK.
        return reinterpret_cast<Value>(in);
    } else if constexpr (std::is_same_v<bool, T>) {
        // make_signed_t<bool> is not defined, so we handle the bool type separately here.
        return static_cast<Value>(in);
    } else if constexpr (std::is_integral_v<T>) {
        // Native integer types are converted to Value using static_cast with sign extension.
        return static_cast<Value>(static_cast<std::make_signed_t<T>>(in));
    }

    Value val{0};
    memcpy(&val, &in, sizeof(T));
    return val;
}

template <typename T>
T bitcastTo(const Value in) noexcept {
    static_assert(std::is_pointer_v<T> || std::is_integral_v<T> || std::is_floating_point_v<T> ||
                  std::is_same_v<Decimal128, T>);

    if constexpr (std::is_pointer_v<T>) {
        // Casting from integer value to pointer is OK.
        static_assert(sizeof(Value) == sizeof(T));
        return reinterpret_cast<T>(in);
    } else if constexpr (std::is_integral_v<T>) {
        // Values are converted to native integer types using static_cast. If sizeof(T) is less
        // than sizeof(Value), the upper bits of 'in' are discarded.
        static_assert(sizeof(Value) >= sizeof(T));
        return static_cast<T>(in);
    } else if constexpr (std::is_same_v<Decimal128, T>) {
        static_assert(sizeof(Value) == sizeof(T*));
        return readDecimal128FromMemory(ConstDataView{getRawPointerView(in)});
    } else {
        static_assert(sizeof(Value) >= sizeof(T));
        T val;
        memcpy(&val, &in, sizeof(T));
        return val;
    }
}

/**
 * Defines hash value for <TypeTags, Value> pair. To be used in associative containers.
 */
struct ValueHash {
    explicit ValueHash(const CollatorInterface* collator = nullptr) : _collator(collator) {}

    size_t operator()(const std::pair<TypeTags, Value>& p) const {
        return hashValue(p.first, p.second, _collator);
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

private:
    const CollatorInterface* _collator;
};

/**
 * Defines equivalence of two <TypeTags, Value> pairs. To be used in associative containers.
 */
struct ValueEq {
    explicit ValueEq(const CollatorInterface* collator = nullptr) : _collator(collator) {}

    bool operator()(const std::pair<TypeTags, Value>& lhs,
                    const std::pair<TypeTags, Value>& rhs) const {
        auto comparator = _collator;

        auto [tag, val] = compareValue(lhs.first, lhs.second, rhs.first, rhs.second, comparator);

        if (tag != TypeTags::NumberInt32 || bitcastTo<int32_t>(val) != 0) {
            return false;
        } else {
            return true;
        }
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

private:
    const CollatorInterface* _collator;
};

template <typename T>
using ValueMapType = absl::flat_hash_map<std::pair<TypeTags, Value>, T, ValueHash, ValueEq>;
using ValueSetType = absl::flat_hash_set<std::pair<TypeTags, Value>, ValueHash, ValueEq>;

/**
 * This is the SBE representation of objects/documents. It is a relatively simple structure of
 * vectors of field names, type tags, and values.
 */
class Object {
public:
    Object() = default;
    Object(const Object& other) {
        // Reserve space in all vectors, they are the same size. We arbitrarily picked _typeTags
        // to determine the size.
        reserve(other._typeTags.size());
        _names = other._names;
        for (size_t idx = 0; idx < other._values.size(); ++idx) {
            const auto [tag, val] = copyValue(other._typeTags[idx], other._values[idx]);
            _values.push_back(val);
            _typeTags.push_back(tag);
        }
    }
    Object(Object&&) = default;
    ~Object() {
        for (size_t idx = 0; idx < _typeTags.size(); ++idx) {
            releaseValue(_typeTags[idx], _values[idx]);
        }
    }

    void push_back(std::string_view name, TypeTags tag, Value val) {
        if (tag != TypeTags::Nothing) {
            ValueGuard guard{tag, val};
            // Reserve space in all vectors, they are the same size. We arbitrarily picked _typeTags
            // to determine the size.
            reserve(_typeTags.size() + 1);
            _names.emplace_back(std::string(name));

            _typeTags.push_back(tag);
            _values.push_back(val);

            guard.reset();
        }
    }

    std::pair<TypeTags, Value> getField(std::string_view field) {
        for (size_t idx = 0; idx < _typeTags.size(); ++idx) {
            if (_names[idx] == field) {
                return {_typeTags[idx], _values[idx]};
            }
        }
        return {TypeTags::Nothing, 0};
    }

    auto size() const noexcept {
        return _values.size();
    }

    auto& field(size_t idx) const {
        return _names[idx];
    }

    std::pair<TypeTags, Value> getAt(std::size_t idx) const {
        if (idx >= _values.size()) {
            return {TypeTags::Nothing, 0};
        }

        return {_typeTags[idx], _values[idx]};
    }

    void reserve(size_t s) {
        // Normalize to at least 1.
        s = s ? s : 1;
        _typeTags.reserve(s);
        _values.reserve(s);
        _names.reserve(s);
    }

private:
    std::vector<TypeTags> _typeTags;
    std::vector<Value> _values;
    std::vector<std::string> _names;
};

/**
 * This is the SBE representation of arrays. It is similar to Object without the field names.
 */
class Array {
public:
    Array() = default;
    Array(const Array& other) {
        // Reserve space in all vectors, they are the same size. We arbitrarily picked _typeTags
        // to determine the size.
        reserve(other._typeTags.size());
        for (size_t idx = 0; idx < other._values.size(); ++idx) {
            const auto [tag, val] = copyValue(other._typeTags[idx], other._values[idx]);
            _values.push_back(val);
            _typeTags.push_back(tag);
        }
    }
    Array(Array&&) = default;
    ~Array() {
        for (size_t idx = 0; idx < _typeTags.size(); ++idx) {
            releaseValue(_typeTags[idx], _values[idx]);
        }
    }

    void push_back(TypeTags tag, Value val) {
        if (tag != TypeTags::Nothing) {
            ValueGuard guard{tag, val};
            // Reserve space in all vectors, they are the same size. We arbitrarily picked _typeTags
            // to determine the size.
            reserve(_typeTags.size() + 1);

            _typeTags.push_back(tag);
            _values.push_back(val);

            guard.reset();
        }
    }

    auto size() const noexcept {
        return _values.size();
    }

    std::pair<TypeTags, Value> getAt(std::size_t idx) const {
        if (idx >= _values.size()) {
            return {TypeTags::Nothing, 0};
        }

        return {_typeTags[idx], _values[idx]};
    }

    void reserve(size_t s) {
        // Normalize to at least 1.
        s = s ? s : 1;
        _typeTags.reserve(s);
        _values.reserve(s);
    }

private:
    std::vector<TypeTags> _typeTags;
    std::vector<Value> _values;
};

/**
 * This is a set of unique values with the same interface as Array.
 */
class ArraySet {
public:
    using iterator = ValueSetType::iterator;

    explicit ArraySet(const CollatorInterface* collator = nullptr)
        : _values(0, ValueHash(collator), ValueEq(collator)) {}

    ArraySet(const ArraySet& other)
        : _values(0, other._values.hash_function(), other._values.key_eq()) {
        reserve(other._values.size());
        for (const auto& p : other._values) {
            const auto copy = copyValue(p.first, p.second);
            ValueGuard guard{copy.first, copy.second};
            _values.insert(copy);
            guard.reset();
        }
    }

    ArraySet(ArraySet&&) = default;

    ~ArraySet() {
        for (const auto& p : _values) {
            releaseValue(p.first, p.second);
        }
    }

    void push_back(TypeTags tag, Value val);

    auto& values() noexcept {
        return _values;
    }

    auto size() const noexcept {
        return _values.size();
    }
    void reserve(size_t s) {
        // Normalize to at least 1.
        s = s ? s : 1;
        _values.reserve(s);
    }

    const CollatorInterface* getCollator() {
        return _values.key_eq().getCollator();
    }

private:
    ValueSetType _values;
};

/**
 * Implements a wrapper of PCRE regular expression.
 * Storing the pattern and the options allows for copying of the sbe::value::PcreRegex expression,
 * which includes recompilation.
 * The compiled expression pcre* allows for direct usage of the pcre C library functionality.
 */
class PcreRegex {
public:
    PcreRegex(std::string_view pattern, std::string_view options)
        : _pattern(pattern), _options(options) {
        _compile();
    }

    PcreRegex(std::string_view pattern) : PcreRegex(pattern, "") {}

    PcreRegex(const PcreRegex& other) : PcreRegex(other._pattern, other._options) {}

    PcreRegex& operator=(const PcreRegex& other) {
        if (this != &other) {
            (*pcre_free)(_pcrePtr);
            _pattern = other._pattern;
            _options = other._options;
            _compile();
        }
        return *this;
    }

    ~PcreRegex() {
        (*pcre_free)(_pcrePtr);
    }

    const std::string& pattern() const {
        return _pattern;
    }

    const std::string& options() const {
        return _options;
    }

    /**
     * Wrapper function for pcre_exec().
     * - input: The input string.
     * - startPos: The position from where the search should start.
     * - buf: Array populated with the found matched string and capture groups.
     * Returns the number of matches or an error code:
     *         < -1 error
     *         = -1 no match
     *         = 0  there was a match, but not enough space in the buffer
     *         > 0  the number of matches
     */
    int execute(std::string_view input, int startPos, std::vector<int>& buf);

    size_t getNumberCaptures() const;

private:
    void _compile();

    std::string _pattern;
    std::string _options;

    pcre* _pcrePtr;
};

constexpr size_t kSmallStringMaxLength = 7;
using ObjectIdType = std::array<uint8_t, 12>;
static_assert(sizeof(ObjectIdType) == 12);

template <typename T>
T readFromMemory(const char* memory) noexcept {
    T val;
    memcpy(&val, memory, sizeof(T));
    return val;
}

template <typename T>
T readFromMemory(const unsigned char* memory) noexcept {
    T val;
    memcpy(&val, memory, sizeof(T));
    return val;
}

template <typename T>
size_t writeToMemory(unsigned char* memory, const T val) noexcept {
    memcpy(memory, &val, sizeof(T));

    return sizeof(T);
}

/**
 * getRawStringView() returns a char* or const char* that points to the first character of a given
 * string (or a null terminator byte if the string is empty). Where possible, getStringView() should
 * be preferred over getRawStringView().
 */
inline char* getRawStringView(TypeTags tag, Value& val) noexcept {
    if (tag == TypeTags::StringSmall) {
        return reinterpret_cast<char*>(&val);
    } else if (tag == TypeTags::StringBig || tag == TypeTags::bsonString) {
        return getRawPointerView(val) + 4;
    }
    MONGO_UNREACHABLE;
}

inline const char* getRawStringView(TypeTags tag, const Value& val) noexcept {
    if (tag == TypeTags::StringSmall) {
        return reinterpret_cast<const char*>(&val);
    } else if (tag == TypeTags::StringBig || tag == TypeTags::bsonString) {
        return getRawPointerView(val) + 4;
    }
    MONGO_UNREACHABLE;
}

/**
 * getStringLength() returns the number of characters in a string (excluding the null terminator).
 */
inline size_t getStringLength(TypeTags tag, const Value& val) noexcept {
    if (tag == TypeTags::StringSmall) {
        return strlen(reinterpret_cast<const char*>(&val));
    } else if (tag == TypeTags::StringBig || tag == TypeTags::bsonString) {
        return ConstDataView(getRawPointerView(val)).read<LittleEndian<int32_t>>() - 1;
    }
    MONGO_UNREACHABLE;
}

/**
 * getStringView() should be preferred over getRawStringView() where possible.
 */
inline std::string_view getStringView(TypeTags tag, const Value& val) noexcept {
    return {getRawStringView(tag, val), getStringLength(tag, val)};
}

inline size_t getBSONBinDataSize(TypeTags tag, Value val) {
    invariant(tag == TypeTags::bsonBinData);
    return static_cast<size_t>(
        ConstDataView(getRawPointerView(val)).read<LittleEndian<uint32_t>>());
}

inline BinDataType getBSONBinDataSubtype(TypeTags tag, Value val) {
    invariant(tag == TypeTags::bsonBinData);
    return static_cast<BinDataType>((getRawPointerView(val) + sizeof(uint32_t))[0]);
}

inline uint8_t* getBSONBinData(TypeTags tag, Value val) {
    invariant(tag == TypeTags::bsonBinData);
    return reinterpret_cast<uint8_t*>(getRawPointerView(val) + sizeof(uint32_t) + 1);
}

/**
 * Same as 'getBsonBinDataSize()' except when the BinData has the 'ByteArrayDeprecated' subtype,
 * in which case it returns the size of the payload, rather than the size of the entire BinData.
 *
 * The BSON spec originally stipulated that BinData values with the "binary" subtype (named
 * 'ByteArrayDeprecated' here) should structure their contents so that the first four bytes store
 * the length of the payload, which occupies the remaining bytes. That subtype is now deprecated,
 * but there are some callers that remain aware of it and operate on the payload rather than the
 * whole BinData byte array. Most callers, however, should use the regular 'getBSONBinDataSize()'
 * and 'getBSONBinData()' and remain oblivious to the BinData subtype.
 *
 * Note that this payload size is computed by subtracting the size of the length bytes from the
 * overall size of BinData. Even though this function supports the deprecated subtype, it still
 * ignores the payload length value.
 */
inline size_t getBSONBinDataSizeCompat(TypeTags tag, Value val) {
    auto size = getBSONBinDataSize(tag, val);
    if (getBSONBinDataSubtype(tag, val) != ByteArrayDeprecated) {
        return size;
    } else {
        return (size >= sizeof(uint32_t)) ? size - sizeof(uint32_t) : 0;
    }
}

/**
 * Same as 'getBsonBinData()' except when the BinData has the 'ByteArrayDeprecated' subtype, in
 * which case it returns a pointer to the payload, rather than a pointer to the beginning of the
 * BinData.
 *
 * See the 'getBSONBinDataSizeCompat()' documentation for an explanation of the
 * 'ByteArrayDeprecated' subtype.
 */
inline uint8_t* getBSONBinDataCompat(TypeTags tag, Value val) {
    auto binData = getBSONBinData(tag, val);
    if (getBSONBinDataSubtype(tag, val) != ByteArrayDeprecated) {
        return binData;
    } else {
        return binData + sizeof(uint32_t);
    }
}

inline bool canUseSmallString(std::string_view input) {
    auto length = input.size();
    auto ptr = input.data();
    auto end = ptr + length;
    return length <= kSmallStringMaxLength && std::find(ptr, end, '\0') == end;
}

/**
 * Callers must check that canUseSmallString() returns true before calling this function.
 * makeNewString() should be preferred over makeSmallString() where possible.
 */
inline std::pair<TypeTags, Value> makeSmallString(std::string_view input) {
    dassert(canUseSmallString(input));

    Value smallString{0};
    auto buf = getRawStringView(TypeTags::StringSmall, smallString);
    memcpy(buf, input.data(), input.size());
    return {TypeTags::StringSmall, smallString};
}

inline std::pair<TypeTags, Value> makeBigString(std::string_view input) {
    auto len = input.size();
    auto ptr = input.data();

    invariant(len < static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));

    auto length = static_cast<uint32_t>(len);
    auto buf = new char[length + 5];
    DataView(buf).write<LittleEndian<int32_t>>(length + 1);
    memcpy(buf + 4, ptr, length);
    buf[length + 4] = 0;
    return {TypeTags::StringBig, reinterpret_cast<Value>(buf)};
}

inline std::pair<TypeTags, Value> makeNewString(std::string_view input) {
    if (canUseSmallString(input)) {
        return makeSmallString(input);
    } else {
        return makeBigString(input);
    }
}

inline std::pair<TypeTags, Value> makeNewArray() {
    auto a = new Array;
    return {TypeTags::Array, reinterpret_cast<Value>(a)};
}

inline std::pair<TypeTags, Value> makeNewArraySet(const CollatorInterface* collator = nullptr) {
    auto a = new ArraySet(collator);
    return {TypeTags::ArraySet, reinterpret_cast<Value>(a)};
}

inline std::pair<TypeTags, Value> makeCopyArray(const Array& inA) {
    auto a = new Array(inA);
    return {TypeTags::Array, reinterpret_cast<Value>(a)};
}

inline std::pair<TypeTags, Value> makeCopyArraySet(const ArraySet& inA) {
    auto a = new ArraySet(inA);
    return {TypeTags::ArraySet, reinterpret_cast<Value>(a)};
}

inline Array* getArrayView(Value val) noexcept {
    return reinterpret_cast<Array*>(val);
}

inline ArraySet* getArraySetView(Value val) noexcept {
    return reinterpret_cast<ArraySet*>(val);
}

inline std::pair<TypeTags, Value> makeNewObject() {
    auto o = new Object;
    return {TypeTags::Object, reinterpret_cast<Value>(o)};
}

inline std::pair<TypeTags, Value> makeCopyObject(const Object& inO) {
    auto o = new Object(inO);
    return {TypeTags::Object, reinterpret_cast<Value>(o)};
}

inline Object* getObjectView(Value val) noexcept {
    return reinterpret_cast<Object*>(val);
}

inline std::pair<TypeTags, Value> makeNewObjectId() {
    auto o = new ObjectIdType;
    return {TypeTags::ObjectId, reinterpret_cast<Value>(o)};
}

inline std::pair<TypeTags, Value> makeCopyObjectId(const ObjectIdType& inO) {
    auto o = new ObjectIdType(inO);
    return {TypeTags::ObjectId, reinterpret_cast<Value>(o)};
}

inline ObjectIdType* getObjectIdView(Value val) noexcept {
    return reinterpret_cast<ObjectIdType*>(val);
}

inline std::pair<TypeTags, Value> makeCopyDecimal(const Decimal128& inD) {
    auto valueBuffer = new char[2 * sizeof(long long)];
    DataView decimalView(valueBuffer);
    decimalView.write<LittleEndian<long long>>(inD.getValue().low64, 0);
    decimalView.write<LittleEndian<long long>>(inD.getValue().high64, sizeof(long long));
    return {TypeTags::NumberDecimal, reinterpret_cast<Value>(valueBuffer)};
}

inline KeyString::Value* getKeyStringView(Value val) noexcept {
    return reinterpret_cast<KeyString::Value*>(val);
}

std::pair<TypeTags, Value> makeNewPcreRegex(std::string_view pattern, std::string_view options);

std::pair<TypeTags, Value> makeCopyPcreRegex(const PcreRegex& regex);

inline PcreRegex* getPcreRegexView(Value val) noexcept {
    return reinterpret_cast<PcreRegex*>(val);
}

inline JsFunction* getJsFunctionView(Value val) noexcept {
    return reinterpret_cast<JsFunction*>(val);
}

inline TimeZoneDatabase* getTimeZoneDBView(Value val) noexcept {
    return reinterpret_cast<TimeZoneDatabase*>(val);
}

inline ShardFilterer* getShardFiltererView(Value val) noexcept {
    return reinterpret_cast<ShardFilterer*>(val);
}

inline CollatorInterface* getCollatorView(Value val) noexcept {
    return reinterpret_cast<CollatorInterface*>(val);
}

/**
 * Pattern and flags of Regex are stored in BSON as two C strings written one after another.
 *
 *   <pattern> <NULL> <flags> <NULL>
 */
struct BsonRegex {
    BsonRegex(const char* rawValue) {
        pattern = rawValue;
        // We add 1 to account NULL byte after pattern.
        flags = pattern.data() + pattern.size() + 1;
    }

    BsonRegex(std::string_view pattern, std::string_view flags) : pattern(pattern), flags(flags) {
        // Ensure that flags follow right after pattern in memory. Otherwise 'dataView()' may return
        // invalid 'std::string_view' object.
        invariant(pattern.data() + pattern.size() + 1 == flags.data());
    }

    size_t byteSize() const {
        // We add 2 to account NULL bytes after each string.
        return pattern.size() + flags.size() + 2;
    }

    const char* data() const {
        return pattern.data();
    }

    std::string_view dataView() const {
        return {data(), byteSize()};
    }

    std::string_view pattern;
    std::string_view flags;
};

inline BsonRegex getBsonRegexView(Value val) noexcept {
    return BsonRegex(getRawPointerView(val));
}

std::pair<TypeTags, Value> makeCopyBsonRegex(const BsonRegex& regex);

inline std::string_view getBsonJavascriptView(Value val) noexcept {
    return getStringView(TypeTags::StringBig, val);
}

std::pair<TypeTags, Value> makeCopyBsonJavascript(std::string_view code);

std::pair<TypeTags, Value> makeCopyKeyString(const KeyString::Value& inKey);

std::pair<TypeTags, Value> makeCopyJsFunction(const JsFunction&);

std::pair<TypeTags, Value> makeCopyShardFilterer(const ShardFilterer&);

void releaseValue(TypeTags tag, Value val) noexcept;

inline std::pair<TypeTags, Value> copyValue(TypeTags tag, Value val) {
    switch (tag) {
        case TypeTags::NumberDecimal:
            return makeCopyDecimal(bitcastTo<Decimal128>(val));
        case TypeTags::Array:
            return makeCopyArray(*getArrayView(val));
        case TypeTags::ArraySet:
            return makeCopyArraySet(*getArraySetView(val));
        case TypeTags::Object:
            return makeCopyObject(*getObjectView(val));
        case TypeTags::StringBig:
            return makeBigString(getStringView(tag, val));
        case TypeTags::bsonString:
            return makeBigString(getStringView(tag, val));
        case TypeTags::ObjectId: {
            return makeCopyObjectId(*getObjectIdView(val));
        }
        case TypeTags::bsonArray:
        case TypeTags::bsonObject: {
            auto bson = getRawPointerView(val);
            auto size = ConstDataView(bson).read<LittleEndian<uint32_t>>();

            // Owned BSON memory is managed through a UniqueBuffer for compatibility
            // with the BSONObj/BSONArray class.
            auto buffer = UniqueBuffer::allocate(size);
            memcpy(buffer.get(), bson, size);
            return {tag, reinterpret_cast<Value>(buffer.release())};
        }
        case TypeTags::bsonObjectId: {
            auto bson = getRawPointerView(val);
            auto size = sizeof(ObjectIdType);
            auto dst = new uint8_t[size];
            memcpy(dst, bson, size);
            return {TypeTags::bsonObjectId, reinterpret_cast<Value>(dst)};
        }
        case TypeTags::bsonBinData: {
            auto binData = getRawPointerView(val);
            auto size = getBSONBinDataSize(tag, val);
            auto dst = new uint8_t[size + sizeof(uint32_t) + 1];
            memcpy(dst, binData, size + sizeof(uint32_t) + 1);
            return {TypeTags::bsonBinData, reinterpret_cast<Value>(dst)};
        }
        case TypeTags::ksValue:
            return makeCopyKeyString(*getKeyStringView(val));
        case TypeTags::pcreRegex:
            return makeCopyPcreRegex(*getPcreRegexView(val));
        case TypeTags::jsFunction:
            return makeCopyJsFunction(*getJsFunctionView(val));
        case TypeTags::shardFilterer:
            return makeCopyShardFilterer(*getShardFiltererView(val));
        case TypeTags::bsonRegex:
            return makeCopyBsonRegex(getBsonRegexView(val));
        case TypeTags::bsonJavascript:
            return makeCopyBsonJavascript(getBsonJavascriptView(val));
        default:
            break;
    }

    return {tag, val};
}

/**
 * Implicit conversions of numerical types.
 */
template <typename T>
inline T numericCast(TypeTags tag, Value val) noexcept {
    switch (tag) {
        case TypeTags::NumberInt32:
            if constexpr (std::is_same_v<T, Decimal128>) {
                return Decimal128(bitcastTo<int32_t>(val));
            } else {
                return bitcastTo<int32_t>(val);
            }
        case TypeTags::NumberInt64:
            if constexpr (std::is_same_v<T, Decimal128>) {
                return Decimal128(bitcastTo<int64_t>(val));
            } else {
                return bitcastTo<int64_t>(val);
            }
        case TypeTags::NumberDouble:
            if constexpr (std::is_same_v<T, Decimal128>) {
                return Decimal128(bitcastTo<double>(val));
            } else {
                return bitcastTo<double>(val);
            }
        case TypeTags::NumberDecimal:
            if constexpr (std::is_same_v<T, Decimal128>) {
                return bitcastTo<Decimal128>(val);
            }
            MONGO_UNREACHABLE;
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Performs a lossless numeric conversion from a value to a destination type denoted by the target
 * TypeTag. In the case that a conversion is lossy, we return Nothing.
 */
template <typename T>
inline std::tuple<bool, value::TypeTags, value::Value> numericConvLossless(
    T value, value::TypeTags targetTag) {
    switch (targetTag) {
        case value::TypeTags::NumberInt32: {
            if (auto result = representAs<int32_t>(value); result) {
                return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(*result)};
            }
            return {false, value::TypeTags::Nothing, 0};
        }
        case value::TypeTags::NumberInt64: {
            if (auto result = representAs<int64_t>(value); result) {
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(*result)};
            }
            return {false, value::TypeTags::Nothing, 0};
        }
        case value::TypeTags::NumberDouble: {
            if (auto result = representAs<double>(value); result) {
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(*result)};
            }
            return {false, value::TypeTags::Nothing, 0};
        }
        case value::TypeTags::NumberDecimal: {
            if (auto result = representAs<Decimal128>(value); result) {
                auto [tag, val] = value::makeCopyDecimal(*result);
                return {true, tag, val};
            }
            return {false, value::TypeTags::Nothing, 0};
        }
        default:
            MONGO_UNREACHABLE
    }
}

inline TypeTags getWidestNumericalType(TypeTags lhsTag, TypeTags rhsTag) noexcept {
    if (lhsTag == TypeTags::NumberDecimal || rhsTag == TypeTags::NumberDecimal) {
        return TypeTags::NumberDecimal;
    } else if (lhsTag == TypeTags::NumberDouble || rhsTag == TypeTags::NumberDouble) {
        return TypeTags::NumberDouble;
    } else if (lhsTag == TypeTags::NumberInt64 || rhsTag == TypeTags::NumberInt64) {
        return TypeTags::NumberInt64;
    } else if (lhsTag == TypeTags::NumberInt32 || rhsTag == TypeTags::NumberInt32) {
        return TypeTags::NumberInt32;
    } else {
        MONGO_UNREACHABLE;
    }
}


class ObjectEnumerator {
public:
    ObjectEnumerator() = default;
    ObjectEnumerator(TypeTags tag, Value val) {
        reset(tag, val);
    }
    void reset(TypeTags tag, Value val) {
        _tagObject = tag;
        _valObject = val;
        _object = nullptr;
        _index = 0;

        if (tag == TypeTags::Object) {
            _object = getObjectView(val);
        } else if (tag == TypeTags::bsonObject) {
            auto bson = getRawPointerView(val);
            _objectCurrent = bson + 4;
            _objectEnd = bson + ConstDataView(bson).read<LittleEndian<uint32_t>>();
        } else {
            MONGO_UNREACHABLE;
        }
    }
    std::pair<TypeTags, Value> getViewOfValue() const;
    std::string_view getFieldName() const;

    bool atEnd() const {
        if (_object) {
            return _index == _object->size();
        } else {
            return *_objectCurrent == 0;
        }
    }

    bool advance();

private:
    TypeTags _tagObject{TypeTags::Nothing};
    Value _valObject{0};

    // Object
    Object* _object{nullptr};
    size_t _index{0};

    // bsonObject
    const char* _objectCurrent{nullptr};
    const char* _objectEnd{nullptr};
};

/**
 * Holds a view of an array-like type (e.g. TypeTags::Array or TypeTags::bsonArray), and provides an
 * iterface to iterate over the values that are the elements of the array.
 */
class ArrayEnumerator {
public:
    ArrayEnumerator() = default;
    ArrayEnumerator(TypeTags tag, Value val) {
        reset(tag, val);
    }

    void reset(TypeTags tag, Value val) {
        _tagArray = tag;
        _valArray = val;
        _array = nullptr;
        _arraySet = nullptr;
        _index = 0;

        if (tag == TypeTags::Array) {
            _array = getArrayView(val);
        } else if (tag == TypeTags::ArraySet) {
            _arraySet = getArraySetView(val);
            _iter = _arraySet->values().begin();
        } else if (tag == TypeTags::bsonArray) {
            auto bson = getRawPointerView(val);
            _arrayCurrent = bson + 4;
            _arrayEnd = bson + ConstDataView(bson).read<LittleEndian<uint32_t>>();
        } else {
            MONGO_UNREACHABLE;
        }
    }

    std::pair<TypeTags, Value> getViewOfValue() const;

    bool atEnd() const {
        if (_array) {
            return _index == _array->size();
        } else if (_arraySet) {
            return _iter == _arraySet->values().end();
        } else {
            return *_arrayCurrent == 0;
        }
    }

    bool advance();

private:
    TypeTags _tagArray{TypeTags::Nothing};
    Value _valArray{0};

    // Array
    Array* _array{nullptr};
    size_t _index{0};

    // ArraySet
    ArraySet* _arraySet{nullptr};
    ArraySet::iterator _iter;

    // bsonArray
    const char* _arrayCurrent{nullptr};
    const char* _arrayEnd{nullptr};
};

/**
 * Copies the content of the input array into an ArraySet. If the input has duplicate elements, they
 * will be removed.
 */
std::pair<TypeTags, Value> arrayToSet(TypeTags tag,
                                      Value val,
                                      CollatorInterface* collator = nullptr);

}  // namespace value
}  // namespace sbe
}  // namespace mongo

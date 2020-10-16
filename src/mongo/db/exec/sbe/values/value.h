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
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/represent_as.h"

namespace pcrecpp {
class RE;
}  // namespace pcrecpp

namespace mongo {
/**
 * Forward declaration.
 */
namespace KeyString {
class Value;
}

class TimeZoneDatabase;

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

    // TODO add the rest of mongo types (regex, etc.)

    // Raw bson values.
    bsonObject,
    bsonArray,
    bsonString,
    bsonObjectId,
    bsonBinData,

    // KeyString::Value
    ksValue,

    // Pointer to a compiled PCRE regular expression object.
    pcreRegex,

    // Pointer to a timezone database object.
    timeZoneDB,
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
std::size_t hashValue(TypeTags tag, Value val) noexcept;

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
std::pair<TypeTags, Value> compareValue(TypeTags lhsTag,
                                        Value lhsValue,
                                        TypeTags rhsTag,
                                        Value rhsValue);

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
    ValueGuard(ValueGuard&&) = delete;
    ~ValueGuard() {
        releaseValue(_tag, _value);
    }

    ValueGuard& operator=(const ValueGuard&) = delete;
    ValueGuard& operator=(ValueGuard&&) = delete;

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
    struct Hash {
        size_t operator()(const std::pair<TypeTags, Value>& p) const {
            return hashValue(p.first, p.second);
        }
    };
    struct Eq {
        bool operator()(const std::pair<TypeTags, Value>& lhs,
                        const std::pair<TypeTags, Value>& rhs) const {
            auto [tag, val] = compareValue(lhs.first, lhs.second, rhs.first, rhs.second);

            if (tag != TypeTags::NumberInt32 || bitcastTo<int32_t>(val) != 0) {
                return false;
            } else {
                return true;
            }
        }
    };
    using SetType = absl::flat_hash_set<std::pair<TypeTags, Value>, Hash, Eq>;

public:
    using iterator = SetType::iterator;

    ArraySet() = default;
    ArraySet(const ArraySet& other) {
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

private:
    SetType _values;
};

constexpr size_t kSmallStringThreshold = 8;
using ObjectIdType = std::array<uint8_t, 12>;
static_assert(sizeof(ObjectIdType) == 12);

inline char* getSmallStringView(Value& val) noexcept {
    return reinterpret_cast<char*>(&val);
}

inline char* getBigStringView(Value val) noexcept {
    return reinterpret_cast<char*>(val);
}

inline char* getRawStringView(TypeTags tag, Value& val) noexcept {
    if (tag == TypeTags::StringSmall) {
        return getSmallStringView(val);
    } else if (tag == TypeTags::StringBig) {
        return getBigStringView(val);
    } else if (tag == TypeTags::bsonString) {
        return getRawPointerView(val) + 4;
    }
    MONGO_UNREACHABLE;
}

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

inline std::string_view getStringView(TypeTags tag, Value& val) noexcept {
    if (tag == TypeTags::StringSmall) {
        return std::string_view(getSmallStringView(val));
    } else if (tag == TypeTags::StringBig) {
        return std::string_view(getBigStringView(val));
    } else if (tag == TypeTags::bsonString) {
        auto bsonstr = getRawPointerView(val);
        return std::string_view(bsonstr + 4,
                                ConstDataView(bsonstr).read<LittleEndian<uint32_t>>() - 1);
    }
    MONGO_UNREACHABLE;
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

inline std::pair<TypeTags, Value> makeSmallString(std::string_view input) {
    size_t len = input.size();
    invariant(len < kSmallStringThreshold - 1);

    Value smallString;
    // This is OK - we are aliasing to char*.
    auto stringAlias = getSmallStringView(smallString);
    memcpy(stringAlias, input.data(), len);
    stringAlias[len] = 0;
    return {TypeTags::StringSmall, smallString};
}

inline std::pair<TypeTags, Value> makeNewString(std::string_view input) {
    size_t len = input.size();
    if (len < kSmallStringThreshold - 1) {
        return makeSmallString(input);
    } else {
        auto str = new char[len + 1];
        memcpy(str, input.data(), len);
        str[len] = 0;
        return {TypeTags::StringBig, reinterpret_cast<Value>(str)};
    }
}

inline std::pair<TypeTags, Value> makeNewArray() {
    auto a = new Array;
    return {TypeTags::Array, reinterpret_cast<Value>(a)};
}

inline std::pair<TypeTags, Value> makeNewArraySet() {
    auto a = new ArraySet;
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

inline pcrecpp::RE* getPcreRegexView(Value val) noexcept {
    return reinterpret_cast<pcrecpp::RE*>(val);
}

inline TimeZoneDatabase* getTimeZoneDBView(Value val) noexcept {
    return reinterpret_cast<TimeZoneDatabase*>(val);
}

std::pair<TypeTags, Value> makeCopyKeyString(const KeyString::Value& inKey);

std::pair<TypeTags, Value> makeCopyPcreRegex(const pcrecpp::RE&);

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
        case TypeTags::StringBig: {
            auto src = getBigStringView(val);
            auto len = strlen(src);
            auto dst = new char[len + 1];
            memcpy(dst, src, len);
            dst[len] = 0;
            return {TypeTags::StringBig, reinterpret_cast<Value>(dst)};
        }
        case TypeTags::bsonString: {
            auto bsonstr = getRawPointerView(val);
            auto src = bsonstr + 4;
            auto size = ConstDataView(bsonstr).read<LittleEndian<uint32_t>>();
            return makeNewString(std::string_view(src, size - 1));
        }
        case TypeTags::ObjectId: {
            return makeCopyObjectId(*getObjectIdView(val));
        }
        case TypeTags::bsonObject: {
            auto bson = getRawPointerView(val);
            auto size = ConstDataView(bson).read<LittleEndian<uint32_t>>();
            auto dst = new uint8_t[size];
            memcpy(dst, bson, size);
            return {TypeTags::bsonObject, reinterpret_cast<Value>(dst)};
        }
        case TypeTags::bsonObjectId: {
            auto bson = getRawPointerView(val);
            auto size = sizeof(ObjectIdType);
            auto dst = new uint8_t[size];
            memcpy(dst, bson, size);
            return {TypeTags::bsonObjectId, reinterpret_cast<Value>(dst)};
        }
        case TypeTags::bsonArray: {
            auto bson = getRawPointerView(val);
            auto size = ConstDataView(bson).read<LittleEndian<uint32_t>>();
            auto dst = new uint8_t[size];
            memcpy(dst, bson, size);
            return {TypeTags::bsonArray, reinterpret_cast<Value>(dst)};
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

}  // namespace value
}  // namespace sbe
}  // namespace mongo

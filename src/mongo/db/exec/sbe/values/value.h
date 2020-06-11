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
#include <cstdint>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"

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
namespace sbe {
using FrameId = int64_t;
using SpoolId = int64_t;

namespace value {
using SlotId = int64_t;

/**
 * Type dispatch tags.
 */
enum class TypeTags : uint8_t {
    // The value does not exist, aka Nothing in the Maybe monad.
    Nothing = 0,

    // Numberical data types.
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

    // KeyString::Value
    ksValue,

    // Pointer to a compiled PCRE regular expression object.
    pcreRegex,
};

std::ostream& operator<<(std::ostream& os, const TypeTags tag);

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
void printValue(std::ostream& os, TypeTags tag, Value val);
std::size_t hashValue(TypeTags tag, Value val) noexcept;

/**
 * Three ways value comparison (aka spaceship operator).
 */
std::pair<TypeTags, Value> compareValue(TypeTags lhsTag,
                                        Value lhsValue,
                                        TypeTags rhsTag,
                                        Value rhsValue);

/**
 * RAII guard.
 */
class ValueGuard {
public:
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

            if (tag != TypeTags::NumberInt32 || val != 0) {
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

inline char* getRawPointerView(Value val) noexcept {
    return reinterpret_cast<char*>(val);
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

template <typename T>
Value bitcastFrom(const T in) noexcept {
    static_assert(sizeof(Value) >= sizeof(T));

    // Casting from pointer to integer value is OK.
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<Value>(in);
    }
    Value val{0};
    memcpy(&val, &in, sizeof(T));
    return val;
}

template <typename T>
T bitcastTo(const Value in) noexcept {
    // Casting from integer value to pointer is OK.
    if constexpr (std::is_pointer_v<T>) {
        static_assert(sizeof(Value) == sizeof(T));
        return reinterpret_cast<T>(in);
    } else if constexpr (std::is_same_v<Decimal128, T>) {
        static_assert(sizeof(Value) == sizeof(T*));
        T val;
        memcpy(&val, getRawPointerView(in), sizeof(T));
        return val;
    } else {
        static_assert(sizeof(Value) >= sizeof(T));
        T val;
        memcpy(&val, &in, sizeof(T));
        return val;
    }
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

inline std::pair<TypeTags, Value> makeNewString(std::string_view input) {
    size_t len = input.size();
    if (len < kSmallStringThreshold - 1) {
        Value smallString;
        // This is OK - we are aliasing to char*.
        auto stringAlias = getSmallStringView(smallString);
        memcpy(stringAlias, input.data(), len);
        stringAlias[len] = 0;
        return {TypeTags::StringSmall, smallString};
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

inline Decimal128* getDecimalView(Value val) noexcept {
    return reinterpret_cast<Decimal128*>(val);
}

inline std::pair<TypeTags, Value> makeCopyDecimal(const Decimal128& inD) {
    auto o = new Decimal128(inD);
    return {TypeTags::NumberDecimal, reinterpret_cast<Value>(o)};
}

inline KeyString::Value* getKeyStringView(Value val) noexcept {
    return reinterpret_cast<KeyString::Value*>(val);
}

inline pcrecpp::RE* getPrceRegexView(Value val) noexcept {
    return reinterpret_cast<pcrecpp::RE*>(val);
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
            return {TypeTags::StringBig, bitcastFrom(dst)};
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
            return {TypeTags::bsonObject, bitcastFrom(dst)};
        }
        case TypeTags::bsonObjectId: {
            auto bson = getRawPointerView(val);
            auto size = sizeof(ObjectIdType);
            auto dst = new uint8_t[size];
            memcpy(dst, bson, size);
            return {TypeTags::bsonObjectId, bitcastFrom(dst)};
        }
        case TypeTags::bsonArray: {
            auto bson = getRawPointerView(val);
            auto size = ConstDataView(bson).read<LittleEndian<uint32_t>>();
            auto dst = new uint8_t[size];
            memcpy(dst, bson, size);
            return {TypeTags::bsonArray, bitcastFrom(dst)};
        }
        case TypeTags::ksValue:
            return makeCopyKeyString(*getKeyStringView(val));
        case TypeTags::pcreRegex:
            return makeCopyPcreRegex(*getPrceRegexView(val));
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

class SlotAccessor {
public:
    virtual ~SlotAccessor() = 0;
    /**
     * Returns a non-owning view of value currently stored in the slot. The returned value is valid
     * until the content of this slot changes (usually as a result of calling getNext()). If the
     * caller needs to hold onto the value longer then it must make a copy of the value.
     */
    virtual std::pair<TypeTags, Value> getViewOfValue() const = 0;

    /**
     * Sometimes it may be determined that a caller is the last one to access this slot. If that is
     * the case then the caller can use this optimized method to move out the value out of the slot
     * saving the extra copy operation. Not all slots own the values stored in them so they must
     * make a deep copy. The returned value is owned by the caller.
     */
    virtual std::pair<TypeTags, Value> copyOrMoveValue() = 0;
};
inline SlotAccessor::~SlotAccessor() {}

class ViewOfValueAccessor final : public SlotAccessor {
public:
    // Return non-owning view of the value.
    std::pair<TypeTags, Value> getViewOfValue() const override {
        return {_tag, _val};
    }

    // Return a copy.
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return copyValue(_tag, _val);
    }

    void reset() {
        reset(TypeTags::Nothing, 0);
    }

    void reset(TypeTags tag, Value val) {
        _tag = tag;
        _val = val;
    }

private:
    TypeTags _tag{TypeTags::Nothing};
    Value _val{0};
};

class OwnedValueAccessor final : public SlotAccessor {
public:
    OwnedValueAccessor() = default;
    OwnedValueAccessor(const OwnedValueAccessor& other) {
        if (other._owned) {
            auto [tag, val] = copyValue(other._tag, other._val);
            _tag = tag;
            _val = val;
            _owned = true;
        } else {
            _tag = other._tag;
            _val = other._val;
            _owned = false;
        }
    }
    OwnedValueAccessor(OwnedValueAccessor&& other) noexcept {
        _tag = other._tag;
        _val = other._val;
        _owned = other._owned;

        other._owned = false;
    }

    ~OwnedValueAccessor() {
        release();
    }

    // Copy and swap idiom for a single copy/move assignment operator.
    OwnedValueAccessor& operator=(OwnedValueAccessor other) noexcept {
        std::swap(_tag, other._tag);
        std::swap(_val, other._val);
        std::swap(_owned, other._owned);
        return *this;
    }

    // Return non-owning view of the value.
    std::pair<TypeTags, Value> getViewOfValue() const override {
        return {_tag, _val};
    }

    std::pair<TypeTags, Value> copyOrMoveValue() override {
        if (_owned) {
            _owned = false;
            return {_tag, _val};
        } else {
            return copyValue(_tag, _val);
        }
    }

    void reset() {
        reset(TypeTags::Nothing, 0);
    }

    void reset(TypeTags tag, Value val) {
        reset(true, tag, val);
    }

    void reset(bool owned, TypeTags tag, Value val) {
        release();

        _tag = tag;
        _val = val;
        _owned = owned;
    }

private:
    void release() {
        if (_owned) {
            releaseValue(_tag, _val);
            _owned = false;
        }
    }

    TypeTags _tag{TypeTags::Nothing};
    Value _val;
    bool _owned{false};
};

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

class ArrayAccessor final : public SlotAccessor {
public:
    void reset(TypeTags tag, Value val) {
        _enumerator.reset(tag, val);
    }

    // Return non-owning view of the value.
    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _enumerator.getViewOfValue();
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        // We can never move out values from array.
        auto [tag, val] = getViewOfValue();
        return copyValue(tag, val);
    }

    bool atEnd() const {
        return _enumerator.atEnd();
    }

    bool advance() {
        return _enumerator.advance();
    }

private:
    ArrayEnumerator _enumerator;
};

struct MaterializedRow {
    std::vector<OwnedValueAccessor> _fields;

    void makeOwned() {
        for (auto& f : _fields) {
            auto [tag, val] = f.getViewOfValue();
            auto [copyTag, copyVal] = copyValue(tag, val);
            f.reset(copyTag, copyVal);
        }
    }
    bool operator==(const MaterializedRow& rhs) const {
        for (size_t idx = 0; idx < _fields.size(); ++idx) {
            auto [lhsTag, lhsVal] = _fields[idx].getViewOfValue();
            auto [rhsTag, rhsVal] = rhs._fields[idx].getViewOfValue();
            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal);

            if (tag != TypeTags::NumberInt32 || val != 0) {
                return false;
            }
        }

        return true;
    }
};

struct MaterializedRowComparator {
    const std::vector<SortDirection>& _direction;
    // TODO - add collator and whatnot.

    MaterializedRowComparator(const std::vector<value::SortDirection>& direction)
        : _direction(direction) {}

    bool operator()(const MaterializedRow& lhs, const MaterializedRow& rhs) const {
        for (size_t idx = 0; idx < lhs._fields.size(); ++idx) {
            auto [lhsTag, lhsVal] = lhs._fields[idx].getViewOfValue();
            auto [rhsTag, rhsVal] = rhs._fields[idx].getViewOfValue();
            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
            if (tag != TypeTags::NumberInt32) {
                return false;
            }
            if (bitcastTo<int32_t>(val) < 0 && _direction[idx] == SortDirection::Ascending) {
                return true;
            }
            if (bitcastTo<int32_t>(val) > 0 && _direction[idx] == SortDirection::Descending) {
                return true;
            }
            if (bitcastTo<int32_t>(val) != 0) {
                return false;
            }
        }

        return false;
    }
};
struct MaterializedRowHasher {
    std::size_t operator()(const MaterializedRow& k) const {
        size_t res = 17;
        for (auto& f : k._fields) {
            auto [tag, val] = f.getViewOfValue();
            res = res * 31 + hashValue(tag, val);
        }
        return res;
    }
};

template <typename T>
class MaterializedRowKeyAccessor final : public SlotAccessor {
public:
    MaterializedRowKeyAccessor(T& it, size_t slot) : _it(it), _slot(slot) {}

    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _it->first._fields[_slot].getViewOfValue();
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        // We can never move out values from keys.
        auto [tag, val] = getViewOfValue();
        return copyValue(tag, val);
    }

private:
    T& _it;
    size_t _slot;
};

template <typename T>
class MaterializedRowValueAccessor final : public SlotAccessor {
public:
    MaterializedRowValueAccessor(T& it, size_t slot) : _it(it), _slot(slot) {}

    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _it->second._fields[_slot].getViewOfValue();
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return _it->second._fields[_slot].copyOrMoveValue();
    }

    void reset(bool owned, TypeTags tag, Value val) {
        _it->second._fields[_slot].reset(owned, tag, val);
    }

private:
    T& _it;
    size_t _slot;
};

template <typename T>
class MaterializedRowAccessor final : public SlotAccessor {
public:
    MaterializedRowAccessor(T& container, const size_t& it, size_t slot)
        : _container(container), _it(it), _slot(slot) {}

    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _container[_it]._fields[_slot].getViewOfValue();
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return _container[_it]._fields[_slot].copyOrMoveValue();
    }

    void reset(bool owned, TypeTags tag, Value val) {
        _container[_it]._fields[_slot].reset(owned, tag, val);
    }

private:
    T& _container;
    const size_t& _it;
    const size_t _slot;
};

/**
 * Commonly used containers
 */
template <typename T>
using SlotMap = absl::flat_hash_map<SlotId, T>;
using SlotAccessorMap = SlotMap<SlotAccessor*>;
using FieldAccessorMap = absl::flat_hash_map<std::string, std::unique_ptr<ViewOfValueAccessor>>;
using SlotSet = absl::flat_hash_set<SlotId>;
using SlotVector = std::vector<SlotId>;

}  // namespace value
}  // namespace sbe
}  // namespace mongo

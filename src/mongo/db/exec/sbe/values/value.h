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
#include <boost/predef/hardware/simd.h>
#include <cstdint>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/fts/fts_matcher.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/endian.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/represent_as.h"

namespace mongo {
/**
 * Forward declarations.
 */
class RecordId;

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
class SortSpec;
class MakeObjSpec;

static constexpr size_t kNewUUIDLength = 16;

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

    // Date data types.
    Date,
    Timestamp,

    Boolean,
    Null,
    StringSmall,

    MinKey,
    MaxKey,

    // Special marker
    EndOfShallowValues = MaxKey,

    // Heap values
    NumberDecimal,
    StringBig,
    Array,
    ArraySet,
    Object,

    ObjectId,
    RecordId,

    // Raw bson values.
    bsonObject,
    bsonArray,
    bsonString,
    bsonSymbol,
    bsonObjectId,
    bsonBinData,
    // The bson prefix signifies the fact that this type can only come from BSON (either from disk
    // or from user over the wire). It is never created or manipulated by SBE.
    bsonUndefined,
    bsonRegex,
    bsonJavascript,
    bsonDBPointer,
    bsonCodeWScope,

    // Local lambda value
    LocalLambda,

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

    // Pointer to fts::FTSMatcher for full text search.
    ftsMatcher,

    // Pointer to a SortSpec object.
    sortSpec,

    // Pointer to a MakeObjSpec object.
    makeObjSpec,

    // Pointer to a IndexBounds object.
    indexBounds,

    // Pointer to a classic engine match expression.
    classicMatchExpresion,
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

inline constexpr bool isBsonRegex(TypeTags tag) noexcept {
    return tag == TypeTags::bsonRegex;
}

inline constexpr bool isStringOrSymbol(TypeTags tag) noexcept {
    return isString(tag) || tag == TypeTags::bsonSymbol;
}

inline constexpr bool isCollatableType(TypeTags tag) noexcept {
    return isString(tag) || isArray(tag) || isObject(tag);
}

inline constexpr bool isShallowType(TypeTags tag) noexcept {
    return tag <= TypeTags::EndOfShallowValues;
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

/**
 * Releases memory allocated for the value. If the value does not have any memory allocated for it,
 * does nothing.
 *
 * NOTE: This function is intentionally marked as 'noexcept' and must not throw. It is used in the
 *       destructors of several classes to implement RAII concept for values.
 */
void releaseValueDeep(TypeTags tag, Value val) noexcept;
std::pair<TypeTags, Value> copyValue(TypeTags tag, Value val);
std::size_t hashValue(TypeTags tag,
                      Value val,
                      const CollatorInterface* collator = nullptr) noexcept;

inline void releaseValue(TypeTags tag, Value val) noexcept {
    if (!isShallowType(tag)) {
        releaseValueDeep(tag, val);
    } else {
        // No action is needed to release "shallow" values.
    }
}

/**
 * Overloads for writing values and tags to stream.
 */
std::ostream& operator<<(std::ostream& os, TypeTags tag);
str::stream& operator<<(str::stream& str, TypeTags tag);
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

bool isInfinity(TypeTags tag, Value val);

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
Value bitcastFrom(
    const dont_deduce<T> in) noexcept {  // NOLINT(readability-avoid-const-params-in-decls)
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
T bitcastTo(const Value in) noexcept {  // NOLINT(readability-avoid-const-params-in-decls)
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

/**
 * 'DeepEqualityHashSet' is a wrapper around 'absl::flat_hash_set' that provides a "truly" deep
 * equality comparison function between its instances. The equality operator in the underlying
 * 'absl::flat_hash_set' type doesn't use the provided equality functor. Instead, it relies on the
 * default comparison function for the key type, which is not preferrable in our usage scenarios.
 * This is the main reason for having the 'DeepEqualityHashSet' wrapper type.
 */
template <class T,
          class Hash = absl::container_internal::hash_default_hash<T>,
          class Eq = absl::container_internal::hash_default_eq<T>,
          class Allocator = std::allocator<T>>
class DeepEqualityHashSet {
public:
    using SetType = absl::flat_hash_set<T, Hash, Eq, Allocator>;
    using iterator = typename SetType::iterator;
    using const_iterator = typename SetType::const_iterator;

    explicit DeepEqualityHashSet(size_t bucket_count, const Hash& hash, const Eq& eq)
        : _values(bucket_count, hash, eq) {}

    Hash hash_function() const {
        return _values.hash_function();
    }
    Eq key_eq() const {
        return _values.key_eq();
    }

    size_t size() const {
        return _values.size();
    }

    void reserve(size_t n) {
        _values.reserve(n);
    }

    std::pair<iterator, bool> insert(const T& value) {
        return _values.insert(value);
    }

    bool contains(const T& key) const {
        return _values.contains(key);
    }

    iterator find(const T& key) {
        return _values.find(key);
    }

    const_iterator find(const T& key) const {
        return _values.find(key);
    }

    size_t count(const T& key) const {
        return _values.count(key);
    }

    iterator begin() {
        return _values.begin();
    }
    iterator end() {
        return _values.end();
    }

    const_iterator begin() const {
        return _values.begin();
    }
    const_iterator end() const {
        return _values.end();
    }

    template <class T1, class Hash1, class Eq1, class Allocator1>
    friend bool operator==(const DeepEqualityHashSet<T1, Hash1, Eq1, Allocator1>& lhs,
                           const DeepEqualityHashSet<T1, Hash1, Eq1, Allocator1>& rhs) {
        using SetTp = typename DeepEqualityHashSet<T1, Hash1, Eq1, Allocator1>::SetType;
        const SetTp* inner = &lhs._values;
        const SetTp* outer = &rhs._values;
        if (outer->size() != inner->size()) {
            return false;
        }

        if (outer->capacity() > inner->capacity()) {
            std::swap(inner, outer);
        }

        for (const auto& e : *outer) {
            // The equality check in the 'absl::flat_hash_set' type doesn't use the provided
            // equality functor. Instead, it relies on the default comparison function for the key
            // type, which is not preferrable in our usage scenarios. This is the main reason for
            // having the 'DeepEqualityHashSet' wrapper type.
            if (!inner->contains(e)) {
                return false;
            }
        }
        return true;
    }

    template <class T1, class Hash1, class Eq1, class Allocator1>
    friend bool operator!=(const DeepEqualityHashSet<T1, Hash1, Eq1, Allocator1>& lhs,
                           const DeepEqualityHashSet<T1, Hash1, Eq1, Allocator1>& rhs) {
        return !(lhs == rhs);
    }

private:
    SetType _values;
};

/**
 * 'DeepEqualityHashMap' is a wrapper around 'absl::flat_hash_map' that provides a "truly" deep
 * equality comparison function between its instances. The equality operator in the underlying
 * 'absl::flat_hash_map' type doesn't use the provided equality functor. Instead, it relies on the
 * default comparison function for the key type, which is not preferrable in our usage scenarios.
 * This is the main reason for having the 'DeepEqualityHashMap' wrapper type.
 */
template <class K,
          class V,
          class Hash = absl::container_internal::hash_default_hash<K>,
          class Eq = absl::container_internal::hash_default_eq<K>,
          class Allocator = std::allocator<std::pair<const K, V>>>
class DeepEqualityHashMap {
public:
    using MapType = absl::flat_hash_map<K, V, ValueHash, ValueEq, Allocator>;
    using iterator = typename MapType::iterator;
    using const_iterator = typename MapType::const_iterator;

    explicit DeepEqualityHashMap(size_t bucket_count, const Hash& hash, const Eq& eq)
        : _values(bucket_count, hash, eq) {}

    Hash hash_function() const {
        return _values.hash_function();
    }
    Eq key_eq() const {
        return _values.key_eq();
    }

    size_t size() const {
        return _values.size();
    }

    void reserve(size_t n) {
        _values.reserve(n);
    }

    std::pair<iterator, bool> insert(const K& key, const V& val) {
        return _values.insert({key, val});
    }

    auto& operator[](const K& key) {
        return _values[key];
    }

    bool contains(const K& key) const {
        return _values.contains(key);
    }

    iterator find(const K& key) {
        return _values.find(key);
    }

    const_iterator find(const K& key) const {
        return _values.find(key);
    }

    size_t count(const K& key) const {
        return _values.count(key);
    }

    iterator begin() {
        return _values.begin();
    }
    iterator end() {
        return _values.end();
    }

    const_iterator begin() const {
        return _values.begin();
    }
    const_iterator end() const {
        return _values.end();
    }

    template <class K1, class V1, class Hash1, class Eq1, class Allocator1>
    friend bool operator==(const DeepEqualityHashMap<K1, V1, Hash1, Eq1, Allocator1>& lhs,
                           const DeepEqualityHashMap<K1, V1, Hash1, Eq1, Allocator1>& rhs) {
        using MapTp = typename DeepEqualityHashMap<K1, V1, Hash1, Eq1, Allocator1>::MapType;
        const MapTp* inner = &lhs._values;
        const MapTp* outer = &rhs._values;
        if (outer->size() != inner->size()) {
            return false;
        }

        if (outer->capacity() > inner->capacity()) {
            std::swap(inner, outer);
        }

        for (const auto& e : *outer) {
            // The equality check in the 'absl::flat_hash_map' type doesn't use the provided
            // equality functor. Instead, it relies on the default comparison function for the key
            // type, which is not preferrable in our usage scenarios. This is the main reason for
            // having the 'DeepEqualityHashMap' wrapper type.
            auto it = inner->find(e.first);
            if (it == inner->end() || it->second != e.second) {
                return false;
            }
        }
        return true;
    }

    template <class K1, class V1, class Hash1, class Eq1, class Allocator1>
    friend bool operator!=(const DeepEqualityHashMap<K1, V1, Hash1, Eq1, Allocator1>& lhs,
                           const DeepEqualityHashMap<K1, V1, Hash1, Eq1, Allocator1>& rhs) {
        return !(lhs == rhs);
    }

private:
    MapType _values;
};

template <typename T>
using ValueMapType = DeepEqualityHashMap<std::pair<TypeTags, Value>, T, ValueHash, ValueEq>;
using ValueSetType = DeepEqualityHashSet<std::pair<TypeTags, Value>, ValueHash, ValueEq>;

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

    void push_back(StringData name, TypeTags tag, Value val) {
        if (tag != TypeTags::Nothing) {
            ValueGuard guard{tag, val};
            // Reserve space in all vectors, they are the same size. We arbitrarily picked _typeTags
            // to determine the size.
            if (_typeTags.capacity() == _typeTags.size()) {
                // Reserve double capacity.
                // Note: we are not concerned about the overflow in the operation below, as the size
                // of 'Value' is 8 bytes. Consequently, the maximum capacity ever is 2^64/8 = 2^61.
                // We can freely shift 2^61 << 1 without any overflow.
                // Note: the case of '_typeTags.capacity() == 1' is handled inside 'reserve' itself.
                reserve(_typeTags.capacity() << 1);
            }
            _names.emplace_back(std::string(name));
            _typeTags.push_back(tag);
            _values.push_back(val);

            guard.reset();
        }
    }

    std::pair<TypeTags, Value> getField(StringData field) {
        for (size_t idx = 0; idx < _typeTags.size(); ++idx) {
            if (_names[idx] == field) {
                return {_typeTags[idx], _values[idx]};
            }
        }
        return {TypeTags::Nothing, 0};
    }

    bool contains(StringData field) const {
        return std::find(_names.begin(), _names.end(), field) != _names.end();
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

    // The in-place update of objects is allowed only in very limited set of contexts (e.g. when
    // objects are used in an accumulator slot). The owner of the object must guarantee that no
    // other component can observe the value being updated.
    void setAt(std::size_t idx, TypeTags tag, Value val) {
        if (tag != TypeTags::Nothing && idx < _values.size()) {
            releaseValue(_typeTags[idx], _values[idx]);
            _typeTags[idx] = tag;
            _values[idx] = val;
        }
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
            if (_typeTags.capacity() == _typeTags.size()) {
                // Reserve double capacity.
                // Note: we are not concerned about the overflow in the operation below, as the size
                // of 'Value' is 8 bytes. Consequently, the maximum capacity ever is 2^64/8 = 2^61.
                // We can freely shift 2^61 << 1 without any overflow.
                // Note: the case of '_typeTags.capacity() == 1' is handled inside 'reserve' itself.
                reserve(_typeTags.capacity() << 1);
            }
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

    // The in-place update of arrays is allowed only in very limited set of contexts (e.g. when
    // arrays are used in an accumulator slot). The owner of the array must guarantee that no other
    // component can observe the value being updated.
    void setAt(std::size_t idx, TypeTags tag, Value val) {
        if (tag != TypeTags::Nothing && idx < _values.size()) {
            releaseValue(_typeTags[idx], _values[idx]);
            _typeTags[idx] = tag;
            _values[idx] = val;
        }
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
    using const_iterator = ValueSetType::const_iterator;

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

    auto& values() const noexcept {
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

bool operator==(const ArraySet& lhs, const ArraySet& rhs);
bool operator!=(const ArraySet& lhs, const ArraySet& rhs);

constexpr size_t kSmallStringMaxLength = 7;
using ObjectIdType = std::array<uint8_t, 12>;
static_assert(sizeof(ObjectIdType) == 12);

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
        // This path turned out to be very hot in our benchmarks, so we avoid calling 'strlen()' and
        // use an alternative approach to compute string length.
        // NOTE: Small string value always contains exactly one zero byte, marking the end of the
        // string. Bytes after this zero byte can have arbitrary value.
#if defined(BOOST_HW_SIMD_X86_AVAILABLE) && BOOST_HW_SIMD_X86 >= BOOST_HW_SIMD_X86_SSE2_VERSION
        // If SSE2 instruction set is available, we use SIMD instructions. There are several steps:
        //  (1) _mm_cvtsi64_si128(val) - Copy string value into the 128-bit register
        //  (2) _mm_cmpeq_epi8 - Make each zero byte equal to 0xFF. Other bytes become zero
        //  (3) _mm_movemask_epi8 - Copy most significant bit of each byte into int
        //  (4) countTrailingZerosNonZeroInt - Get the position of the first trailing bit set
        static_assert(endian::Order::kNative == endian::Order::kLittle);
        int ret = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_cvtsi64_si128(val), _mm_setzero_si128()));
        return countTrailingZerosNonZero32(ret);
#else
        // If SSE2 is not available, we use bit magic.
        const uint64_t magic = 0x7F7F7F7F7F7F7F7FULL;

        // This is based on a trick from following link, which describes how to make an expression
        // which results in '0' when ALL bytes are non-zero, and results in zero when ANY byte is
        // zero. Instead of casting this result to bool, we count how many complete 0 bytes there
        // are in 'ret'. This tells us the length of the string.
        // https://graphics.stanford.edu/~seander/bithacks.html#ZeroInWord

        // At the end of this, ret will store a value where  each byte which was all 0's is now
        // 10000000 and any byte that was anything non zero is now 0.

        // (1) compute (val & magic). This clears the highest bit in each byte.
        uint64_t ret = val & magic;

        // (2) add magic to the above expression. The result is that, from overflow,
        // the high bit will be set if any bit was set in 'v' other than the high bit.
        ret += magic;
        // (3) OR this result with v. This ensures that if the high bit in 'v' was set
        // then the high bit in our result will be set.
        ret |= val;
        // (4) OR with magic. This will set any low bits which were not already set. At this point
        // each byte is either all ones or 01111111.
        ret |= magic;

        // When we invert this, each byte will either be
        // all zeros (previously non zero) or 10000000 (previously 0).
        ret = ~ret;

        // So all we have to do is count how many complete 0 bytes there are from one end.
        if constexpr (endian::Order::kNative == endian::Order::kLittle) {
            return (countTrailingZerosNonZero64(ret) >> 3);
        } else {
            return (countLeadingZerosNonZero64(ret) >> 3);
        }
#endif
    } else if (tag == TypeTags::StringBig || tag == TypeTags::bsonString) {
        return ConstDataView(getRawPointerView(val)).read<LittleEndian<int32_t>>() - 1;
    }
    MONGO_UNREACHABLE;
}

/**
 * getStringView() should be preferred over getRawStringView() where possible.
 */
inline StringData getStringView(TypeTags tag, const Value& val) noexcept {
    return {getRawStringView(tag, val), getStringLength(tag, val)};
}

inline StringData getStringOrSymbolView(TypeTags tag, const Value& val) noexcept {
    tag = (tag == TypeTags::bsonSymbol) ? TypeTags::StringBig : tag;
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

inline RecordId* getRecordIdView(Value val) noexcept {
    return reinterpret_cast<RecordId*>(val);
}

std::pair<TypeTags, Value> makeNewRecordId(int64_t rid);
std::pair<TypeTags, Value> makeNewRecordId(const char* str, int32_t size);
std::pair<TypeTags, Value> makeCopyRecordId(const RecordId&);

inline bool canUseSmallString(StringData input) {
    auto length = input.size();
    auto ptr = input.rawData();
    auto end = ptr + length;
    return length <= kSmallStringMaxLength && std::find(ptr, end, '\0') == end;
}

/**
 * Callers must check that canUseSmallString() returns true before calling this function.
 * makeNewString() should be preferred over makeSmallString() where possible.
 */
inline std::pair<TypeTags, Value> makeSmallString(StringData input) {
    dassert(canUseSmallString(input));

    Value smallString{0};
    auto buf = getRawStringView(TypeTags::StringSmall, smallString);
    memcpy(buf, input.rawData(), input.size());
    return {TypeTags::StringSmall, smallString};
}

inline std::pair<TypeTags, Value> makeBigString(StringData input) {
    auto len = input.size();
    auto ptr = input.rawData();

    invariant(len < static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));

    auto length = static_cast<uint32_t>(len);
    auto buf = new char[length + 5];
    DataView(buf).write<LittleEndian<int32_t>>(length + 1);
    memcpy(buf + 4, ptr, length);
    buf[length + 4] = 0;
    return {TypeTags::StringBig, reinterpret_cast<Value>(buf)};
}

inline std::pair<TypeTags, Value> makeNewString(StringData input) {
    if (canUseSmallString(input)) {
        return makeSmallString(input);
    } else {
        return makeBigString(input);
    }
}

inline std::pair<TypeTags, Value> makeNewBsonSymbol(StringData input) {
    auto [_, strVal] = makeBigString(input);
    return {TypeTags::bsonSymbol, strVal};
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

std::pair<TypeTags, Value> makeNewPcreRegex(StringData pattern, StringData options);

std::pair<TypeTags, Value> makeCopyPcreRegex(const pcre::Regex& regex);

inline pcre::Regex* getPcreRegexView(Value val) noexcept {
    return reinterpret_cast<pcre::Regex*>(val);
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

inline fts::FTSMatcher* getFtsMatcherView(Value val) noexcept {
    return reinterpret_cast<fts::FTSMatcher*>(val);
}

inline SortSpec* getSortSpecView(Value val) noexcept {
    return reinterpret_cast<SortSpec*>(val);
}

inline MakeObjSpec* getMakeObjSpecView(Value val) noexcept {
    return reinterpret_cast<MakeObjSpec*>(val);
}

inline IndexBounds* getIndexBoundsView(Value val) noexcept {
    return reinterpret_cast<IndexBounds*>(val);
}

inline MatchExpression* getClassicMatchExpressionView(Value val) noexcept {
    return reinterpret_cast<MatchExpression*>(val);
}

/**
 * Pattern and flags of Regex are stored in BSON as two C strings written one after another.
 *
 *   <pattern> <NULL> <flags> <NULL>
 */
struct BsonRegex {
    explicit BsonRegex(const char* rawValue) {
        pattern = rawValue;
        // Add one to account for the NULL byte after 'pattern'.
        flags = rawValue + (pattern.size() + 1);
    }

    size_t byteSize() const {
        // Add 2 * sizeof(char) to account for the NULL bytes after 'pattern' and 'flags'.
        return pattern.size() + sizeof(char) + flags.size() + sizeof(char);
    }

    StringData pattern;
    StringData flags;
};

inline BsonRegex getBsonRegexView(Value val) noexcept {
    return BsonRegex(getRawPointerView(val));
}

std::pair<TypeTags, Value> makeNewBsonRegex(StringData pattern, StringData flags);

inline std::pair<TypeTags, Value> makeCopyBsonRegex(const BsonRegex& regex) {
    return makeNewBsonRegex(regex.pattern, regex.flags);
}

inline StringData getBsonJavascriptView(Value val) noexcept {
    return getStringView(TypeTags::StringBig, val);
}

std::pair<TypeTags, Value> makeCopyBsonJavascript(StringData code);

/**
 * The BsonDBPointer class is used to represent the DBRef BSON type. DBRefs consist of a namespace
 * string ('ns') and a document ID ('id'). The namespace string ('ns') can either just specify a
 * collection name (ex. "c"), or it can specify both a database name and a collection name separated
 * by a dot (ex. "db.c").
 *
 * In BSON, a DBRef is encoded as a bsonString ('ns') followed by an ObjectId ('id').
 */
struct BsonDBPointer {
    explicit BsonDBPointer(const char* rawValue) {
        uint32_t lenWithNull = ConstDataView(rawValue).read<LittleEndian<uint32_t>>();
        ns = {rawValue + sizeof(uint32_t), lenWithNull - sizeof(char)};
        id = reinterpret_cast<const uint8_t*>(rawValue) + sizeof(uint32_t) + lenWithNull;
    }

    size_t byteSize() const {
        // Add sizeof(char) to account for the NULL byte after 'ns'.
        return sizeof(uint32_t) + ns.size() + sizeof(char) + sizeof(value::ObjectIdType);
    }

    StringData ns;
    const uint8_t* id{nullptr};
};

inline BsonDBPointer getBsonDBPointerView(Value val) noexcept {
    return BsonDBPointer(getRawPointerView(val));
}

std::pair<TypeTags, Value> makeNewBsonDBPointer(StringData ns, const uint8_t* id);

inline std::pair<TypeTags, Value> makeCopyBsonDBPointer(const BsonDBPointer& dbptr) {
    return makeNewBsonDBPointer(dbptr.ns, dbptr.id);
}

/**
 * The BsonCodeWScope class is used to represent the CodeWScope BSON type.
 *
 * In BSON, a CodeWScope is encoded as a little-endian 32-bit integer ('numBytes'), followed by a
 * bsonString ('code'), followed by a bsonObject ('scope').
 */
struct BsonCodeWScope {
    explicit BsonCodeWScope(const char* rawValue) {
        auto dataView = ConstDataView(rawValue);

        numBytes = dataView.read<LittleEndian<uint32_t>>();
        uint32_t lenWithNull = dataView.read<LittleEndian<uint32_t>>(sizeof(uint32_t));

        auto ptr = rawValue + 2 * sizeof(uint32_t);
        code = {ptr, lenWithNull - sizeof(char)};
        scope = ptr + lenWithNull;
    }

    size_t byteSize() const {
        return numBytes;
    }

    uint32_t numBytes{0};
    StringData code;
    const char* scope{nullptr};
};

inline BsonCodeWScope getBsonCodeWScopeView(Value val) noexcept {
    return BsonCodeWScope(getRawPointerView(val));
}

std::pair<TypeTags, Value> makeNewBsonCodeWScope(StringData code, const char* scope);

inline std::pair<TypeTags, Value> makeCopyBsonCodeWScope(const BsonCodeWScope& cws) {
    return makeNewBsonCodeWScope(cws.code, cws.scope);
}

std::pair<TypeTags, Value> makeCopyKeyString(const KeyString::Value& inKey);

std::pair<TypeTags, Value> makeCopyJsFunction(const JsFunction&);

std::pair<TypeTags, Value> makeCopyShardFilterer(const ShardFilterer&);

std::pair<TypeTags, Value> makeCopyFtsMatcher(const fts::FTSMatcher&);

std::pair<TypeTags, Value> makeCopySortSpec(const SortSpec&);

std::pair<TypeTags, Value> makeCopyMakeObjSpec(const MakeObjSpec&);

std::pair<TypeTags, Value> makeCopyCollator(const CollatorInterface& collator);

std::pair<TypeTags, Value> makeCopyIndexBounds(const IndexBounds& collator);

inline std::pair<TypeTags, Value> copyValue(TypeTags tag, Value val) {
    switch (tag) {
        case TypeTags::RecordId:
            return makeCopyRecordId(*getRecordIdView(val));
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
        case TypeTags::bsonSymbol:
            return makeNewBsonSymbol(getStringOrSymbolView(tag, val));
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
        case TypeTags::bsonDBPointer:
            return makeCopyBsonDBPointer(getBsonDBPointerView(val));
        case TypeTags::bsonCodeWScope:
            return makeCopyBsonCodeWScope(getBsonCodeWScopeView(val));
        case TypeTags::ftsMatcher:
            return makeCopyFtsMatcher(*getFtsMatcherView(val));
        case TypeTags::sortSpec:
            return makeCopySortSpec(*getSortSpecView(val));
        case TypeTags::makeObjSpec:
            return makeCopyMakeObjSpec(*getMakeObjSpecView(val));
        case TypeTags::collator:
            return makeCopyCollator(*getCollatorView(val));
        case TypeTags::indexBounds:
            return makeCopyIndexBounds(*getIndexBoundsView(val));
        case TypeTags::classicMatchExpresion:
            // Beware: "shallow cloning" a match expression does not copy the underlying BSON. The
            // original BSON must remain alive for both the original MatchExpression and the clone.
            return {TypeTags::classicMatchExpresion,
                    bitcastFrom<const MatchExpression*>(
                        getClassicMatchExpressionView(val)->shallowClone().release())};
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
    StringData getFieldName() const;

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

    void reset(TypeTags tag, Value val, size_t index = 0) {
        _tagArray = tag;
        _valArray = val;
        _array = nullptr;
        _arraySet = nullptr;
        _index = 0;

        if (tag == TypeTags::Array) {
            _array = getArrayView(val);
            _index = index;
        } else {
            if (tag == TypeTags::ArraySet) {
                _arraySet = getArraySetView(val);
                _iter = _arraySet->values().begin();
            } else if (tag == TypeTags::bsonArray) {
                auto bson = getRawPointerView(val);
                _arrayCurrent = bson + 4;
                _arrayEnd = bson + ConstDataView(bson).read<LittleEndian<uint32_t>>();
            } else {
                MONGO_UNREACHABLE;
            }

            for (size_t i = 0; !atEnd() && i < index; i++) {
                advance();
            }
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
    ArraySet::const_iterator _iter;

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

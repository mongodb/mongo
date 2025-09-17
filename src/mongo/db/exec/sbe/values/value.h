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
#include <boost/predef/hardware/simd.h>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
// IWYU pragma: no_include "boost/predef/hardware/simd/x86.h"
// IWYU pragma: no_include "boost/predef/hardware/simd/x86/versions.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
// IWYU pragma: no_include "emmintrin.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/ordering.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/values/key_string_entry.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/fts/fts_matcher.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/endian.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mongo {
/**
 * Forward declarations.
 */
class RecordId;

class TimeZoneDatabase;
class TimeZone;

class JsFunction;

namespace sbe {
/**
 * Trivially copyable variation on a tuple theme. This allow us to return tuples through registers.
 */
template <typename...>
struct FastTuple;

template <typename... Ts>
FastTuple(Ts...) -> FastTuple<Ts...>;

template <typename A, typename B, typename C>
struct FastTuple<A, B, C> {
    A a;
    B b;
    C c;
};

struct MakeObjSpec;
class SortSpec;
class InList;

using FrameId = int64_t;
using SpoolId = int64_t;

static constexpr int64_t kInvalidId = LLONG_MIN;

using IndexKeysInclusionSet = std::bitset<Ordering::kMaxCompoundIndexKeys>;

namespace value {
struct ValueBlock;
struct CellBlock;

static constexpr size_t kNewUUIDLength = 16;

/**
 * Type dispatch tags.
 *
 * There are two kinds of SBE types: native types and extended types. In the enum below, native
 * types are listed first followed by extended types, with 'EndOfNativeTypeTags' marking the
 * boundary between the two.
 *
 * The 'sbe_values' module take a link-time dependency on the implementations for native types but
 * not for extended types.
 *
 * Extended types cannot be used with value::compareValue() or value::hashValue(). Also, for any
 * SBE type 'tag', if 'value::tagToType(tag) != EOO' is true then 'tag' must be a native type.
 * Likewise, if 'tag' is an extended type then 'value::tagToType(tag) == EOO' must be true.
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

    MinKey,
    MaxKey,

    // Pointer to sort key component vector. This type is always owned within a SortSpec,
    // and is never created, copied, or destroyed by SBE.
    sortKeyComponentVector,

    // TODO SERVER-95276: Remove this.
    csiCell,

    StringSmall,

    // Special marker
    EndOfShallowTypeTags = StringSmall,

    // Heap values
    NumberDecimal,
    StringBig,
    Array,
    ArraySet,
    ArrayMultiSet,
    Object,
    MultiMap,

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

    // The index key string.
    keyString,

    // Pointer to a timezone database object.
    timeZoneDB,

    // Pointer to a timezone object
    timeZone,

    // Pointer to a collator interface object.
    collator,

    // Pointer to a ValueBlock object.
    valueBlock,

    // Pointer to a CellBlock object.
    cellBlock,

    // Special marker
    EndOfNativeTypeTags = cellBlock,

    // Pointer to a compiled PCRE regular expression object.
    pcreRegex,

    // Pointer to a compiled JS function with scope.
    jsFunction,

    // Pointer to a ShardFilterer for shard filtering.
    shardFilterer,

    // Pointer to an fts::FTSMatcher object for full text search.
    ftsMatcher,

    // Pointer to a SortSpec object.
    sortSpec,

    // Pointer to a MakeObjSpec object.
    makeObjSpec,

    // Pointer to an IndexBounds object.
    indexBounds,

    // Pointer to an InList object.
    inList,

    // Special marker, must be last.
    TypeTagsMax,
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
    return tag == TypeTags::Array || tag == TypeTags::ArraySet || tag == TypeTags::ArrayMultiSet ||
        tag == TypeTags::bsonArray;
}

inline constexpr bool isContainer(TypeTags tag) noexcept {
    return isObject(tag) || isArray(tag) || tag == TypeTags::MultiMap;
}

inline constexpr bool isInList(TypeTags tag) noexcept {
    return tag == TypeTags::inList;
}

inline constexpr bool isNullish(TypeTags tag) noexcept {
    return tag == TypeTags::Nothing || tag == TypeTags::Null || tag == TypeTags::bsonUndefined;
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

inline constexpr bool isTimeZone(TypeTags tag) noexcept {
    return tag == TypeTags::timeZone;
}

inline constexpr bool isStringOrSymbol(TypeTags tag) noexcept {
    return isString(tag) || tag == TypeTags::bsonSymbol;
}

inline constexpr bool isCollatableType(TypeTags tag) noexcept {
    return isStringOrSymbol(tag) || isArray(tag) || isObject(tag);
}

inline constexpr bool isShallowType(TypeTags tag) noexcept {
    return tag <= TypeTags::EndOfShallowTypeTags;
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
 * Functions for writing values and tags to a std::string.
 */
std::string print(const std::pair<TypeTags, Value>& value);           // production function
std::string printTagAndVal(TypeTags tag, Value value);                // debugging function
std::string printTagAndVal(const std::pair<TypeTags, Value>& value);  // debugging function

/**
 * Performs a three-way comparison for any type that has < and == operators. Additionally,
 * guarantees that the result will be exactlty -1, 0, or 1, which is important, because not all
 * comparison functions make that guarantee.
 *
 * The StringData::compare(basic_string_view s) function, for example, only promises that it
 * will return a value less than 0 in the case that 'this' is less than 's,' whereas we want to
 * return exactly -1.
 */
template <typename T>
int32_t compareHelper(const T lhs, const T rhs) noexcept {
    return lhs < rhs ? -1 : (lhs == rhs ? 0 : 1);
}

/**
 * Three ways value comparison (aka spaceship operator).
 */
std::pair<TypeTags, Value> compareValue(TypeTags lhsTag,
                                        Value lhsValue,
                                        TypeTags rhsTag,
                                        Value rhsValue,
                                        const StringDataComparator* comparator = nullptr);

inline std::pair<TypeTags, Value> compare3way(TypeTags lhsTag,
                                              Value lhsValue,
                                              TypeTags rhsTag,
                                              Value rhsValue,
                                              const StringDataComparator* comparator = nullptr) {
    if (lhsTag == TypeTags::Nothing || rhsTag == TypeTags::Nothing) {
        return {TypeTags::Nothing, 0};
    }
    return compareValue(lhsTag, lhsValue, rhsTag, rhsValue, comparator);
}

bool isNaN(TypeTags tag, Value val) noexcept;

bool isInfinity(TypeTags tag, Value val) noexcept;

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
    MONGO_COMPILER_ALWAYS_INLINE ValueGuard(const std::pair<TypeTags, Value> typedValue)
        : ValueGuard(typedValue.first, typedValue.second) {}
    MONGO_COMPILER_ALWAYS_INLINE ValueGuard(TypeTags tag, Value val) : _tag(tag), _value(val) {}
    MONGO_COMPILER_ALWAYS_INLINE ValueGuard(bool owned, TypeTags tag, Value val)
        : ValueGuard(owned ? tag : TypeTags::Nothing, owned ? val : 0) {}
    MONGO_COMPILER_ALWAYS_INLINE ValueGuard(
        const FastTuple<bool, value::TypeTags, value::Value>& tuple)
        : ValueGuard(tuple.a, tuple.b, tuple.c) {}
    ValueGuard() = delete;
    ValueGuard(const ValueGuard&) = delete;
    ValueGuard(ValueGuard&& other) = delete;
    MONGO_COMPILER_ALWAYS_INLINE ~ValueGuard() {
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

/**
 * The same as ValueGuard, but with a move constructor, move assignment operator, and default
 * constructor.
 */
class MoveableValueGuard {
public:
    MONGO_COMPILER_ALWAYS_INLINE MoveableValueGuard(const std::pair<TypeTags, Value> typedValue)
        : MoveableValueGuard(typedValue.first, typedValue.second) {}
    MONGO_COMPILER_ALWAYS_INLINE MoveableValueGuard(TypeTags tag, Value val)
        : _tag(tag), _value(val) {}
    MONGO_COMPILER_ALWAYS_INLINE MoveableValueGuard(bool owned, TypeTags tag, Value val)
        : MoveableValueGuard(owned ? tag : TypeTags::Nothing, owned ? val : 0) {}
    MONGO_COMPILER_ALWAYS_INLINE MoveableValueGuard(
        const FastTuple<bool, value::TypeTags, value::Value>& tuple)
        : MoveableValueGuard(tuple.a, tuple.b, tuple.c) {}
    MoveableValueGuard() {
        disown();
    };
    MoveableValueGuard(const MoveableValueGuard&) = delete;
    MoveableValueGuard(MoveableValueGuard&& other) : _tag(other._tag), _value(other._value) {
        other.disown();
    }
    MONGO_COMPILER_ALWAYS_INLINE ~MoveableValueGuard() {
        releaseValue(_tag, _value);
    }

    MoveableValueGuard& operator=(const MoveableValueGuard&) = delete;
    MoveableValueGuard& operator=(MoveableValueGuard&& other) {
        if (this != &other) {
            releaseValue(_tag, _value);
            _tag = other._tag;
            _value = other._value;
            other.disown();
        }
        return *this;
    }
    std::pair<TypeTags, Value> get() const {
        return {_tag, _value};
    }
    TypeTags tag() const {
        return _tag;
    }
    Value value() const {
        return _value;
    }

    // Used when ownership needs to be transferred away from this MovableValueGuard.
    void disown() {
        _tag = TypeTags::Nothing;
        _value = 0;
    }

private:
    TypeTags _tag;
    Value _value;
};

class ValueVectorGuard {
public:
    MONGO_COMPILER_ALWAYS_INLINE ValueVectorGuard(std::vector<TypeTags>& tags,
                                                  std::vector<Value>& values)
        : _owned(true), _tags(tags), _values(values) {}
    ValueVectorGuard() = delete;
    ValueVectorGuard(const ValueVectorGuard&) = delete;
    ValueVectorGuard(ValueVectorGuard&& other) = delete;
    MONGO_COMPILER_ALWAYS_INLINE ~ValueVectorGuard() {
        if (_owned) {
            invariant(_tags.size() == _values.size());
            for (size_t i = 0; i < _tags.size(); i++) {
                releaseValue(_tags[i], _values[i]);
            }
        }
    }

    ValueVectorGuard& operator=(const ValueVectorGuard&) = delete;
    ValueVectorGuard& operator=(ValueVectorGuard&& other) = delete;

    void reset() {
        _owned = false;
    }

private:
    bool _owned;
    std::vector<TypeTags>& _tags;
    std::vector<Value>& _values;
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
 * Defines less or greater, depending on the template instantiation, of two <TypeTags, Value> pairs.
 * To be used in associative containers.
 */
template <bool less>
struct ValueCompare {
    explicit ValueCompare(const CollatorInterface* collator = nullptr) : _collator(collator) {}

    bool operator()(const std::pair<TypeTags, Value>& lhs,
                    const std::pair<TypeTags, Value>& rhs) const {
        auto [tag, val] = compareValue(lhs.first, lhs.second, rhs.first, rhs.second, _collator);
        uassert(7548805, "Invalid comparison result", tag == TypeTags::NumberInt32);
        if constexpr (less) {
            return bitcastTo<int>(val) < 0;
        } else {
            return bitcastTo<int>(val) > 0;
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

    /**
     * Specialized insert operation that can seek the value and, if it is not present in the set,
     * invoke the keyConstructor function to provide the actual data to be inserted. It assumes that
     * the data to be inserted is identical to the key that has been searched.
     * The main purpose is to allow a copy-on-insert operation.
     */
    std::pair<iterator, bool> insert_lazy(const T& value, std::function<T()> keyConstructor) {
        bool inserted = false;
        auto it = _values.lazy_emplace(value, [&](const SetType::constructor& ctor) {
            inserted = true;
            ctor(keyConstructor());
        });
        return {it, inserted};
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

    void erase(const_iterator pos) {
        _values.erase(pos);
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
        reserve(other._vals.size());
        for (size_t idx = 0; idx < other._vals.size(); ++idx) {
            auto [t, v] = other._vals[idx];
            _vals.push_back(copyValue(t, v));
        }
    }
    Array(Array&&) = default;
    ~Array() {
        for (size_t idx = 0; idx < _vals.size(); ++idx) {
            releaseValue(_vals[idx].first, _vals[idx].second);
        }
    }

    void push_back(TypeTags tag, Value val) {
        if (tag != TypeTags::Nothing) {
            ValueGuard guard{tag, val};
            MONGO_COMPILER_DIAGNOSTIC_PUSH
            MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL("-Wstringop-overflow")
            _vals.push_back({tag, val});
            MONGO_COMPILER_DIAGNOSTIC_POP
            guard.reset();
        }
    }

    void push_back(std::pair<TypeTags, Value> val) {
        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL("-Wstringop-overflow")
        push_back(val.first, val.second);
        MONGO_COMPILER_DIAGNOSTIC_POP
    }

    void pop_back() {
        if (_vals.size() > 0) {
            releaseValue(_vals.back().first, _vals.back().second);
            _vals.pop_back();
        }
    }

    auto size() const noexcept {
        return _vals.size();
    }

    std::pair<TypeTags, Value> getAt(std::size_t idx) const {
        if (idx >= _vals.size()) {
            return {TypeTags::Nothing, 0};
        }

        return _vals[idx];
    }

    std::pair<TypeTags, Value> swapAt(std::size_t idx, TypeTags tag, Value val) {
        if (idx >= _vals.size() || tag == TypeTags::Nothing) {
            return {TypeTags::Nothing, 0};
        }

        auto ret = _vals[idx];
        _vals[idx].first = tag;
        _vals[idx].second = val;
        return ret;
    }

    auto& values() const noexcept {
        return _vals;
    }

    auto& values() noexcept {
        return _vals;
    }


    // The in-place update of arrays is allowed only in very limited set of contexts (e.g. when
    // arrays are used in an accumulator slot). The owner of the array must guarantee that no other
    // component can observe the value being updated.
    void setAt(std::size_t idx, TypeTags tag, Value val) {
        if (tag != TypeTags::Nothing && idx < _vals.size()) {
            releaseValue(_vals[idx].first, _vals[idx].second);
            _vals[idx] = {tag, val};
        }
    }

    void reserve(size_t s) {
        // Normalize to at least 1.
        s = s ? s : 1;
        _vals.reserve(s);
    }

    void clear() {
        for (auto [tag, val] : _vals) {
            releaseValue(tag, val);
        }
        _vals.clear();
    }

private:
    std::vector<std::pair<TypeTags, Value>> _vals;
};

class ArrayMultiSet;

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

    /**
     * Adds the given SBE value to the set if an equal value is not already present. Assumes
     * ownership of the given value.
     *
     * Returns true if the value was newly inserted, otherwise returns false to indicate that an
     * equal value was already present in the set.
     */
    bool push_back(TypeTags tag, Value val);

    bool push_back(std::pair<TypeTags, Value> val) {
        return push_back(val.first, val.second);
    }

    /**
     * If the value cannot be found in the set, insert a copy.
     *
     * Returns true if the value was newly inserted, otherwise returns false to indicate that
     * an equal value was already present in the set.
     */
    bool push_back_clone(TypeTags tag, Value val);

    auto& values() const noexcept {
        return _values;
    }

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
 * This is the SBE representation of multiset. It is similar to ArraySet but each value can be
 * stored multiple times.
 */
class ArrayMultiSet {
public:
    using ValueMultiSetType =
        std::multiset<std::pair<TypeTags, Value>, ValueCompare<true>>;  // NOLINT
    using iterator = typename ValueMultiSetType::iterator;
    using const_iterator = typename ValueMultiSetType::const_iterator;

    ArrayMultiSet() = default;
    explicit ArrayMultiSet(const CollatorInterface* collator = nullptr)
        : _values(ValueCompare<true>(collator)) {}

    ArrayMultiSet(const ArrayMultiSet& other) : _values(ValueCompare<true>(other.getCollator())) {
        for (const auto& p : other._values) {
            const auto copy = copyValue(p.first, p.second);
            ValueGuard guard{copy.first, copy.second};
            _values.insert(copy);
            guard.reset();
        }
    }
    ArrayMultiSet(ArrayMultiSet&&) = default;
    ~ArrayMultiSet() {
        for (auto [tag, val] : _values) {
            releaseValue(tag, val);
        }
        _values.clear();
    }

    /**
     * Adds the given SBE value to the multiset. Assumes ownership of the given value.
     */
    void push_back(TypeTags tag, Value val) {
        if (tag != TypeTags::Nothing) {
            ValueGuard guard{tag, val};
            _values.insert({tag, val});
            guard.reset();
        }
    }

    void push_back(std::pair<TypeTags, Value> val) {
        push_back(val.first, val.second);
    }

    /**
     * Removes an element val from the multiset. Returns false if it was not possible to remove the
     * element.
     */
    bool remove(std::pair<TypeTags, Value> val) {
        // Remove Nothing is always succesful since ArrayMultiset ignores those elements.
        if (val.first == TypeTags::Nothing) {
            return true;
        }

        // We cannot remove an element that is not present.
        if (_values.find(val) == _values.end()) {
            return false;
        }

        auto it = _values.equal_range(val).first;
        releaseValue(it->first, it->second);
        _values.erase(it);
        return true;
    }

    bool remove(TypeTags tag, Value val) {
        return remove({tag, val});
    }

    auto size() const noexcept {
        return _values.size();
    }

    auto& values() const noexcept {
        return _values;
    }

    auto& values() noexcept {
        return _values;
    }

    void clearValues() {
        _values.clear();
    }

    void clear() {
        for (auto [tag, val] : _values) {
            releaseValue(tag, val);
        }
        _values.clear();
    }

    const CollatorInterface* getCollator() const {
        return _values.key_comp().getCollator();
    }

    friend bool operator==(const ArrayMultiSet& lhs, const ArrayMultiSet& rhs) {
        return lhs._values == rhs._values;
    }

    friend bool operator!=(const ArrayMultiSet& lhs, const ArrayMultiSet& rhs) {
        return !(lhs == rhs);
    }

    friend class ArraySet;

private:
    ValueMultiSetType _values;
};

/**
 * This is SBE representation of std::multimap
 */
class MultiMap {
public:
    MultiMap(const CollatorInterface* collator = nullptr) : _values(ValueCompare<true>(collator)) {}

    MultiMap(const MultiMap& other) : _values(ValueCompare<true>(other.getCollator())) {
        for (const auto& [key, value] : other._values) {
            const auto copyKey = copyValue(key.first, key.second);
            const auto copyVal = copyValue(value.first, value.second);
            ValueGuard keyGuard{copyKey.first, copyKey.second};
            ValueGuard valueGuard{copyVal.first, copyVal.second};
            _values.insert({copyKey, copyVal});
            keyGuard.reset();
            valueGuard.reset();
        }
    }

    MultiMap(MultiMap&&) = default;

    ~MultiMap() {
        for (auto& [key, value] : _values) {
            releaseValue(key.first, key.second);
            releaseValue(value.first, value.second);
        }
        _values.clear();
    }

    void insert(std::pair<TypeTags, Value> key, std::pair<TypeTags, Value> value) {
        ValueGuard keyGuard{key};
        ValueGuard valueGuard{value};
        if (key.first != TypeTags::Nothing && value.first != TypeTags::Nothing) {
            _values.insert({key, value});
            keyGuard.reset();
            valueGuard.reset();
        }
    }

    // Remove the entry corresponding to the provided key. In case of multiple equivalent keys, the
    // first entry in the order of insertion will be removed
    bool remove(std::pair<TypeTags, Value> key) {
        if (key.first != TypeTags::Nothing) {
            if (auto it = _values.find(key); it != _values.end()) {
                it = _values.lower_bound(key);
                value::releaseValue(it->first.first, it->first.second);
                value::releaseValue(it->second.first, it->second.second);
                _values.erase(it);
                return true;
            }
        }
        return false;
    }

    auto size() const noexcept {
        return _values.size();
    }

    auto& values() const noexcept {
        return _values;
    }

    auto& values() noexcept {
        return _values;
    }

    const CollatorInterface* getCollator() const {
        return _values.key_comp().getCollator();
    }

private:
    std::multimap<std::pair<TypeTags, Value>, std::pair<TypeTags, Value>, ValueCompare<true>>
        _values;
};

/**
 * A vector of values representing a sort key. The values are NOT owned by this object.
 */
struct SortKeyComponentVector {
    std::vector<std::pair<value::TypeTags, value::Value>> elts;
};

bool operator==(const ArraySet& lhs, const ArraySet& rhs);
bool operator!=(const ArraySet& lhs, const ArraySet& rhs);

bool operator==(const MultiMap& lhs, const MultiMap& rhs);
bool operator!=(const MultiMap& lhs, const MultiMap& rhs);

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

inline size_t getStringOrSymbolLength(TypeTags tag, const Value& val) noexcept {
    tag = (tag == TypeTags::bsonSymbol) ? TypeTags::StringBig : tag;
    return getStringLength(tag, val);
}

/*
 * Using MONGO_COMPILER_ALWAYS_INLINE on a free function does not always play well between
 * compilers because some require the 'inline' keyword be used while others prohibit it. To get
 * around this, we wrap the custom strlen() function in a struct.
 */
struct TinyStrHelpers {
    // Often calling the shared library strlen() function is more expensive than a small loop
    // for small strings.
    MONGO_COMPILER_ALWAYS_INLINE static size_t strlen(const char* s) {
        const char* begin = s;
        while (*s++)
            ;
        return s - begin - 1;
    }
};

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
    uint8_t subtype =
        ConstDataView(getRawPointerView(val) + sizeof(uint32_t)).read<LittleEndian<uint8_t>>();
    return static_cast<BinDataType>(subtype);
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

inline ValueBlock* getValueBlock(Value v) {
    return reinterpret_cast<ValueBlock*>(v);
}

inline CellBlock* getCellBlock(Value v) {
    return reinterpret_cast<CellBlock*>(v);
}

inline bool canUseSmallString(StringData input) {
    auto length = input.size();
    auto ptr = input.data();
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
    tassert(9462500, "'input.data()' can't be a nullptr", input.data());
    memcpy(buf, input.data(), input.size());
    return {TypeTags::StringSmall, smallString};
}

inline std::pair<TypeTags, Value> makeBigString(StringData input) {
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

/**
 * Variant of makeNewArraySet that initializes the set using the data in the provided value. Throws
 * an error if the value is not a type of array.
 */
std::pair<TypeTags, Value> makeNewArraySet(TypeTags tag,
                                           Value value,
                                           const CollatorInterface* collator = nullptr);

inline std::pair<TypeTags, Value> makeNewArrayMultiSet(
    const CollatorInterface* collator = nullptr) {
    auto a = new ArrayMultiSet(collator);
    return {TypeTags::ArrayMultiSet, reinterpret_cast<Value>(a)};
}

inline std::pair<TypeTags, Value> makeCopyArray(const Array& inA) {
    auto a = new Array(inA);
    return {TypeTags::Array, reinterpret_cast<Value>(a)};
}

inline std::pair<TypeTags, Value> makeCopyArraySet(const ArraySet& inA) {
    auto a = new ArraySet(inA);
    return {TypeTags::ArraySet, reinterpret_cast<Value>(a)};
}

inline std::pair<TypeTags, Value> makeCopyArrayMultiSet(const ArrayMultiSet& inA) {
    auto a = new ArrayMultiSet(inA);
    return {TypeTags::ArrayMultiSet, reinterpret_cast<Value>(a)};
}

inline Array* getArrayView(Value val) noexcept {
    return reinterpret_cast<Array*>(val);
}

inline ArraySet* getArraySetView(Value val) noexcept {
    return reinterpret_cast<ArraySet*>(val);
}

inline ArrayMultiSet* getArrayMultiSetView(Value val) noexcept {
    return reinterpret_cast<ArrayMultiSet*>(val);
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

inline std::pair<TypeTags, Value> makeNewMultiMap(const CollatorInterface* collator = nullptr) {
    auto m = new MultiMap(collator);
    return {TypeTags::MultiMap, reinterpret_cast<Value>(m)};
}

inline MultiMap* getMultiMapView(Value val) noexcept {
    return reinterpret_cast<MultiMap*>(val);
}

inline std::pair<TypeTags, Value> makeCopyMultiMap(const MultiMap& map) {
    auto m = new MultiMap(map);
    return {TypeTags::MultiMap, reinterpret_cast<Value>(m)};
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

inline std::pair<TypeTags, Value> makeIntOrLong(int64_t longVal) {
    if ((int32_t)longVal == longVal) {
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>((int32_t)longVal)};
    }
    return {TypeTags::NumberInt64, bitcastFrom<int64_t>(longVal)};
}

inline InList* getInListView(Value val) noexcept {
    return reinterpret_cast<InList*>(val);
}

inline key_string::Value* getKeyStringView(Value val) noexcept {
    return reinterpret_cast<key_string::Value*>(val);
}

inline value::KeyStringEntry* getKeyString(Value val) noexcept {
    return reinterpret_cast<value::KeyStringEntry*>(val);
}

std::pair<TypeTags, Value> makeCopyTimeZone(const TimeZone& tz);

std::pair<TypeTags, Value> makeCopyValueBlock(const ValueBlock& block);

std::pair<TypeTags, Value> makeCopyCellBlock(const CellBlock& block);

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

inline SortKeyComponentVector* getSortKeyComponentVectorView(Value v) noexcept {
    return reinterpret_cast<SortKeyComponentVector*>(v);
}

inline TimeZone* getTimeZoneView(Value val) noexcept {
    return reinterpret_cast<TimeZone*>(val);
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

std::pair<TypeTags, Value> makeKeyString(std::unique_ptr<key_string::Value> inKey);
std::pair<TypeTags, Value> makeKeyString(const key_string::Value& inKey);

std::pair<TypeTags, Value> makeCopyCollator(const CollatorInterface& collator);

struct ExtendedTypeOps {
    std::pair<TypeTags, Value> (*const makeCopy)(Value val);
    void (*const release)(Value val);
    std::string (*const print)(Value val);
    size_t (*const getApproximateSize)(Value val);
};

const ExtendedTypeOps* getExtendedTypeOps(TypeTags tag);

void registerExtendedTypeOps(TypeTags tag, const ExtendedTypeOps* typeOps);

#if defined(MONGO_CONFIG_DEBUG_BUILD)
/**
 * Returns a poison value that should never be encountered in production.
 * Used by asserts/invariants to invalidate values that should never be accessed.
 */
inline std::pair<TypeTags, Value> getPoisonValue() {
    return {TypeTags::Nothing, (uint64_t)-1};
}

inline bool isPoisonValue(TypeTags tag, Value val) {
    return tag == TypeTags::Nothing && val == (uint64_t)-1;
}
#endif

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
        case TypeTags::ArrayMultiSet:
            return makeCopyArrayMultiSet(*getArrayMultiSetView(val));
        case TypeTags::Object:
            return makeCopyObject(*getObjectView(val));
        case TypeTags::MultiMap:
            return makeCopyMultiMap(*getMultiMapView(val));
        case TypeTags::StringBig:
            return makeBigString(getStringView(tag, val));
        case TypeTags::bsonString:
            return makeNewString(getStringView(tag, val));
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
        case TypeTags::bsonRegex:
            return makeCopyBsonRegex(getBsonRegexView(val));
        case TypeTags::bsonJavascript:
            return makeCopyBsonJavascript(getBsonJavascriptView(val));
        case TypeTags::bsonDBPointer:
            return makeCopyBsonDBPointer(getBsonDBPointerView(val));
        case TypeTags::bsonCodeWScope:
            return makeCopyBsonCodeWScope(getBsonCodeWScopeView(val));
        case TypeTags::collator:
            return makeCopyCollator(*getCollatorView(val));
        case TypeTags::timeZone:
            return makeCopyTimeZone(*getTimeZoneView(val));
        case TypeTags::valueBlock:
            return makeCopyValueBlock(*getValueBlock(val));
        case TypeTags::cellBlock:
            return makeCopyCellBlock(*getCellBlock(val));
        case TypeTags::pcreRegex:
        case TypeTags::jsFunction:
        case TypeTags::shardFilterer:
        case TypeTags::ftsMatcher:
        case TypeTags::sortSpec:
        case TypeTags::makeObjSpec:
        case TypeTags::indexBounds:
        case TypeTags::inList:
            return getExtendedTypeOps(tag)->makeCopy(val);
        case TypeTags::keyString:
            return {TypeTags::keyString,
                    bitcastFrom<value::KeyStringEntry*>(getKeyString(val)->makeCopy().release())};
        default:
            break;
    }

    return {tag, val};
}

/**
 * Implicit conversion from any type to a boolean value.
 */
inline std::pair<TypeTags, Value> coerceToBool(TypeTags tag, Value val) {
    switch (tag) {
        case value::TypeTags::Nothing: {
            return {value::TypeTags::Nothing, 0};
        }
        case value::TypeTags::Null:
        case value::TypeTags::bsonUndefined: {
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(false)};
        }
        case value::TypeTags::Boolean: {
            return {tag, val};
        }
        case value::TypeTags::NumberInt32: {
            bool isNotZero = (value::bitcastTo<int32_t>(val) != 0);
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(isNotZero)};
        }
        case value::TypeTags::NumberInt64: {
            bool isNotZero = (value::bitcastTo<int64_t>(val) != 0);
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(isNotZero)};
        }
        case value::TypeTags::NumberDouble: {
            bool isNotZero = (value::bitcastTo<double>(val) != 0.0);
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(isNotZero)};
        }
        case value::TypeTags::NumberDecimal: {
            bool isNotZero = !value::bitcastTo<Decimal128>(val).isZero();
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(isNotZero)};
        }
        default: {
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(true)};
        }
    }
}

/**
 * Convert a numeric value to double, with potential precision loss.
 */
inline std::pair<TypeTags, Value> coerceToDouble(TypeTags tag, Value val) {
    switch (tag) {
        case value::TypeTags::NumberInt32: {
            auto doubleVal = static_cast<double>(value::bitcastTo<int32_t>(val));
            return {value::TypeTags::NumberDouble, value::bitcastFrom<double>(doubleVal)};
        }
        case value::TypeTags::NumberInt64: {
            auto doubleVal = static_cast<double>(value::bitcastTo<int64_t>(val));
            return {value::TypeTags::NumberDouble, value::bitcastFrom<double>(doubleVal)};
        }
        case value::TypeTags::NumberDouble: {
            return {tag, val};
        }
        case value::TypeTags::NumberDecimal: {
            auto doubleVal = value::bitcastTo<Decimal128>(val).toDouble();
            return {value::TypeTags::NumberDouble, value::bitcastFrom<double>(doubleVal)};
        }
        default: {
            return {value::TypeTags::Nothing, 0};
        }
    }
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
inline FastTuple<bool, value::TypeTags, value::Value> numericConvLossless(
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
 *
 * This is a general purpose iterator. If you need to do a simple walk over the entire array in one
 * go, not saving the place across function calls etc, prefer walkArray().
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
                _arraySetIter = _arraySet->values().begin();
            } else if (tag == TypeTags::ArrayMultiSet) {
                _arrayMultiSet = getArrayMultiSetView(val);
                _arrayMultiSetIter = _arrayMultiSet->values().begin();
            } else if (tag == TypeTags::bsonArray) {
                auto bson = getRawPointerView(val);
                _arrayCurrent = bson + 4;
                _arrayEnd = bson + ConstDataView(bson).read<LittleEndian<uint32_t>>();
                if (_arrayCurrent != _arrayEnd - 1) {
                    _fieldNameSize = strlen(_arrayCurrent + 1);
                }
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
            return _arraySetIter == _arraySet->values().end();
        } else if (_arrayMultiSet) {
            return _arrayMultiSetIter == _arrayMultiSet->values().end();
        } else {
            return _arrayCurrent == _arrayEnd - 1;
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
    ArraySet::const_iterator _arraySetIter;

    // ArrayMultiSet
    ArrayMultiSet* _arrayMultiSet{nullptr};
    ArrayMultiSet::const_iterator _arrayMultiSetIter;

    // bsonArray
    const char* _arrayCurrent{nullptr};
    const char* _arrayEnd{nullptr};
    size_t _fieldNameSize = 0;
};

/**
 * Copies the content of the input array into an ArraySet. If the input has duplicate elements, they
 * will be removed.
 */
std::pair<TypeTags, Value> arrayToSet(TypeTags tag,
                                      Value val,
                                      CollatorInterface* collator = nullptr);

std::pair<TypeTags, Value> genericEq(TypeTags lhsTag,
                                     Value lhsValue,
                                     TypeTags rhsTag,
                                     Value rhsValue,
                                     const StringDataComparator* comparator = nullptr);

std::pair<TypeTags, Value> genericNeq(TypeTags lhsTag,
                                      Value lhsValue,
                                      TypeTags rhsTag,
                                      Value rhsValue,
                                      const StringDataComparator* comparator = nullptr);

std::pair<TypeTags, Value> genericLt(TypeTags lhsTag,
                                     Value lhsValue,
                                     TypeTags rhsTag,
                                     Value rhsValue,
                                     const StringDataComparator* comparator = nullptr);

std::pair<TypeTags, Value> genericLte(TypeTags lhsTag,
                                      Value lhsValue,
                                      TypeTags rhsTag,
                                      Value rhsValue,
                                      const StringDataComparator* comparator = nullptr);

std::pair<TypeTags, Value> genericGt(TypeTags lhsTag,
                                     Value lhsValue,
                                     TypeTags rhsTag,
                                     Value rhsValue,
                                     const StringDataComparator* comparator = nullptr);

std::pair<TypeTags, Value> genericGte(TypeTags lhsTag,
                                      Value lhsValue,
                                      TypeTags rhsTag,
                                      Value rhsValue,
                                      const StringDataComparator* comparator = nullptr);
}  // namespace value
}  // namespace sbe
}  // namespace mongo

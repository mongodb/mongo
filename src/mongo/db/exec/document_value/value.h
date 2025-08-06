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

#include "mongo/base/data_range.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/value_internal.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace mongo {
class BSONElement;

/** A variant type that can hold any type of data representable in BSON
 *
 *  Small values are stored inline, but some values, such as large strings,
 *  are heap allocated. It has smart pointer capabilities built-in so it is
 *  safe and recommended to pass these around and return them by value.
 *
 *  Values are immutable, but can be assigned. This means that once you have
 *  a Value, you can be assured that none of the data in that Value will
 *  change. However if you have a non-const Value you replace it with
 *  operator=. These rules are the same as BSONObj, and similar to
 *  shared_ptr<const Object> with stronger guarantees of constness. This is
 *  also the same as Java's std::string type.
 *
 *  Thread-safety: A single Value instance can be safely shared between
 *  threads as long as there are no writers while other threads are
 *  accessing the object. Any number of threads can read from a Value
 *  concurrently. There are no restrictions on how threads access Value
 *  instances exclusively owned by them, even if they reference the same
 *  storage as Value in other threads.
 */
class Value {
public:
    /**
     * Operator overloads for relops return a DeferredComparison which can subsequently be evaluated
     * by a ValueComparator.
     */
    struct DeferredComparison {
        enum class Type {
            kLT,
            kLTE,
            kEQ,
            kGT,
            kGTE,
            kNE,
        };

        DeferredComparison(Type type, const Value& lhs, const Value& rhs)
            : type(type), lhs(lhs), rhs(rhs) {}

        Type type;
        const Value& lhs;
        const Value& rhs;
    };

    /** Construct a Value
     *
     *  All types not listed will be rejected rather than converted, to prevent unexpected implicit
     *  conversions to the accepted argument types (e.g. bool accepts any pointer).
     *
     *  Note: Currently these are all explicit conversions.
     *        I'm not sure if we want implicit or not.
     *  //TODO decide
     */

    Value() : _storage() {}  // "Missing" value
    explicit Value(bool value) : _storage(BSONType::boolean, value) {}
    explicit Value(int value) : _storage(BSONType::numberInt, value) {}
    explicit Value(long long value) : _storage(BSONType::numberLong, value) {}
    explicit Value(double value) : _storage(BSONType::numberDouble, value) {}
    explicit Value(const Decimal128& value) : _storage(BSONType::numberDecimal, value) {}
    explicit Value(const Timestamp& value) : _storage(BSONType::timestamp, value) {}
    explicit Value(const OID& value) : _storage(BSONType::oid, value) {}
    explicit Value(StringData value) : _storage(BSONType::string, value) {}
    explicit Value(const std::string& value) : _storage(BSONType::string, StringData(value)) {}
    explicit Value(const Document& doc);
    explicit Value(Document&& doc);
    explicit Value(const BSONObj& obj);
    explicit Value(const BSONArray& arr);
    explicit Value(const std::vector<BSONObj>& vec);
    explicit Value(const std::vector<Document>& vec);
    explicit Value(std::vector<Value> vec)
        : _storage(BSONType::array, make_intrusive<RCVector<Value>>(std::move(vec))) {}
    explicit Value(const BSONBinData& bd) : _storage(BSONType::binData, bd) {}
    explicit Value(const BSONRegEx& re) : _storage(BSONType::regEx, re) {}
    explicit Value(const BSONCodeWScope& cws) : _storage(BSONType::codeWScope, cws) {}
    explicit Value(const BSONDBRef& dbref) : _storage(BSONType::dbRef, dbref) {}
    explicit Value(const BSONSymbol& sym) : _storage(BSONType::symbol, sym.symbol) {}
    explicit Value(const BSONCode& code) : _storage(BSONType::code, code.code) {}
    explicit Value(const NullLabeler&) : _storage(BSONType::null) {}            // BSONNull
    explicit Value(const UndefinedLabeler&) : _storage(BSONType::undefined) {}  // BSONUndefined
    explicit Value(const MinKeyLabeler&) : _storage(BSONType::minKey) {}        // MINKEY
    explicit Value(const MaxKeyLabeler&) : _storage(BSONType::maxKey) {}        // MAXKEY
    explicit Value(const Date_t& date) : _storage(BSONType::date, date.toMillisSinceEpoch()) {}
    explicit Value(const UUID& uuid)
        : _storage(BSONType::binData,
                   BSONBinData(uuid.toCDR().data(), uuid.toCDR().length(), BinDataType::newUUID)) {}

    /**
     *  Force the use of StringData to prevent accidental NUL-termination.
     */
    explicit Value(const char*) = delete;

    Value(const Value&) = default;
    Value(Value&&) = default;
    Value& operator=(const Value&) = default;
    Value& operator=(Value&&) = default;

    /**
     *  Prevent implicit conversions to the accepted argument types.
     */
    template <typename InvalidArgumentType>
    explicit Value(const InvalidArgumentType&) = delete;


    // TODO: add an unsafe version that can share storage with the BSONElement
    /// Deep-convert from BSONElement to Value
    explicit Value(const BSONElement& elem);

    /// Create a value from a SafeNum.
    explicit Value(const SafeNum& value);


    /** Construct a long or integer-valued Value.
     *
     *  Used when preforming arithmetic operations with int where the
     *  result may be too large and need to be stored as long. The Value
     *  will be an int if value fits, otherwise it will be a long.
     */
    static Value createIntOrLong(long long value);

    /** A "missing" value indicates the lack of a Value.
     *  This is similar to undefined/null but should not appear in output to BSON.
     *  Missing Values are returned by Document when accessing non-existent fields.
     */
    bool missing() const {
        return _storage.type == stdx::to_underlying(BSONType::eoo);
    }

    /// true if missing() or type is jstNULL or Undefined
    bool nullish() const {
        return missing() || _storage.type == stdx::to_underlying(BSONType::null) ||
            _storage.type == stdx::to_underlying(BSONType::undefined);
    }

    /// true if type represents a number
    bool numeric() const {
        return _storage.type == stdx::to_underlying(BSONType::numberDouble) ||
            _storage.type == stdx::to_underlying(BSONType::numberLong) ||
            _storage.type == stdx::to_underlying(BSONType::numberInt) ||
            _storage.type == stdx::to_underlying(BSONType::numberDecimal);
    }

    /**
     * Return true if the Value is an array.
     */
    bool isArray() const {
        return _storage.type == stdx::to_underlying(BSONType::array);
    }

    /**
     * Returns true if this value is a numeric type that can be represented as a 32-bit integer,
     * and false otherwise.
     */
    bool integral() const;

    /**
     * Returns true if this value is numeric and a NaN value.
     */
    bool isNaN() const;

    /**
     * Returns true if this value is numeric and infinite.
     */
    bool isInfinite() const;

    /**
     * Returns true if this value is a numeric type that can be represented as a 64-bit integer,
     * and false otherwise.
     */
    bool integral64Bit() const;

    /**
     * Returns true if this value can be coerced to a Date, and false otherwise.
     */
    bool coercibleToDate() const {
        return BSONType::date == getType() || BSONType::timestamp == getType() ||
            BSONType::oid == getType();
    }

    bool isObject() const {
        return getType() == BSONType::object;
    }

    /// Get the BSON type of the field.
    BSONType getType() const {
        return _storage.bsonType();
    }

    /** Exact type getters.
     *  Asserts if the requested value type is not exactly correct.
     *  See coerceTo methods below for a more type-flexible alternative.
     */
    Decimal128 getDecimal() const;
    double getDouble() const;
    std::string getString() const;
    // May contain embedded NUL bytes, the returned StringData is just a view into the string still
    // owned by this Value.
    StringData getStringData() const;
    Document getDocument() const;
    OID getOid() const;
    bool getBool() const;
    Date_t getDate() const;
    Timestamp getTimestamp() const;
    const char* getRegex() const;
    const char* getRegexFlags() const;
    std::string getSymbol() const;
    std::string getCode() const;
    int getInt() const;
    long long getLong() const;
    UUID getUuid() const;
    // The returned BSONBinData remains owned by this Value.
    BSONBinData getBinData() const;
    const std::vector<Value>& getArray() const {
        return _storage.getArray();
    }
    size_t getArrayLength() const;

    /// Access an element of a subarray. Returns Value() if missing or getType() != Array
    Value operator[](size_t index) const;

    /// Access a field of a subdocument. Returns Value() if missing or getType() != Object
    Value operator[](StringData name) const;

    /**
     * Recursively serializes this value as a field in the object in 'builder' with the field name
     * 'fieldName'. This function throws a AssertionException if the recursion exceeds the server's
     * BSON depth limit.
     */
    void addToBsonObj(BSONObjBuilder* builder,
                      StringData fieldName,
                      size_t recursionLevel = 1) const;

    /**
     * Recursively serializes this value as an element in the array in 'builder'. This function
     * throws a AssertionException if the recursion exceeds the server's BSON depth limit.
     */
    void addToBsonArray(BSONArrayBuilder* builder, size_t recursionLevel = 1) const;

    /** Coerce a value to a bool using BSONElement::trueValue() rules.
     */
    bool coerceToBool() const;

    /** Coercion operators to extract values with fuzzy type logic.
     *
     *  These currently assert if called on an unconvertible type.
     *  TODO: decided how to handle unsupported types.
     */
    std::string coerceToString() const;
    int coerceToInt() const;
    long long coerceToLong() const;
    double coerceToDouble() const;
    Decimal128 coerceToDecimal() const;
    Timestamp coerceToTimestamp() const;
    Date_t coerceToDate() const;

    //
    // Comparison API.
    //
    // Value instances can be compared either using Value::compare() or via operator overloads.
    // Most callers should prefer operator overloads. Note that the operator overloads return a
    // DeferredComparison, which must be subsequently evaluated by a ValueComparator. See
    // value_comparator.h for details.
    //

    /**
     * Compare two Values. Most Values should prefer to use ValueComparator instead. See
     * value_comparator.h for details.
     *
     *  Pass a non-null StringDataComparator if special string comparison semantics are
     *  required. If the comparator is null, then a simple binary compare is used for strings. This
     *  comparator is only used for string *values*; field names are always compared using simple
     *  binary compare.
     *
     *  @returns an integer less than zero, zero, or an integer greater than
     *           zero, depending on whether lhs < rhs, lhs == rhs, or lhs > rhs
     *  Warning: may return values other than -1, 0, or 1
     */
    static int compare(const Value& lhs,
                       const Value& rhs,
                       const StringDataComparator* stringComparator);

    // Support BSONObjBuilder and BSONArrayBuilder "stream" API
    friend void appendToBson(BSONObjBuilder& builder, StringData fieldName, const Value& val) {
        val._appendToBson(builder, fieldName);
    }

    friend DeferredComparison operator==(const Value& lhs, const Value& rhs) {
        return DeferredComparison(DeferredComparison::Type::kEQ, lhs, rhs);
    }

    friend DeferredComparison operator!=(const Value& lhs, const Value& rhs) {
        return DeferredComparison(DeferredComparison::Type::kNE, lhs, rhs);
    }

    friend DeferredComparison operator<(const Value& lhs, const Value& rhs) {
        return DeferredComparison(DeferredComparison::Type::kLT, lhs, rhs);
    }

    friend DeferredComparison operator<=(const Value& lhs, const Value& rhs) {
        return DeferredComparison(DeferredComparison::Type::kLTE, lhs, rhs);
    }

    friend DeferredComparison operator>(const Value& lhs, const Value& rhs) {
        return DeferredComparison(DeferredComparison::Type::kGT, lhs, rhs);
    }

    friend DeferredComparison operator>=(const Value& lhs, const Value& rhs) {
        return DeferredComparison(DeferredComparison::Type::kGTE, lhs, rhs);
    }

    /// This is for debugging, logging, etc. See getString() for how to extract a string.
    std::string toString() const;
    friend std::ostream& operator<<(std::ostream& out, const Value& v);

    /**
     * Returns a cache-only copy of the value with no backing bson.
     */
    Value shred() const;

    void swap(Value& rhs) {
        _storage.swap(rhs._storage);
    }

    /** Figure out what the widest of two numeric types is.
     *
     *  Widest can be thought of as "most capable," or "able to hold the
     *  largest or most precise value."  The progression is Int, Long, Double.
     */
    static BSONType getWidestNumeric(BSONType lType, BSONType rType);

    /// Get the approximate memory size of the value, in bytes. Includes sizeof(Value)
    size_t getApproximateSize() const;

    /**
     * Returns object/array depth of this value. Returns -1 if the depth is at least 'maxDepth'.
     * Returns 0 on a scalar value.
     */
    int32_t depth(int32_t maxDepth, int32_t curDepth = 0) const;

    /**
     * Calculate a hash value.
     *
     * Meant to be used to create composite hashes suitable for hashed container classes such as
     * unordered_map<>.
     *
     * Most callers should prefer the utilities in ValueComparator for hashing and creating function
     * objects for computing the hash. See value_comparator.h.
     */
    void hash_combine(size_t& seed, const StringDataComparator* stringComparator) const;

    /// Call this after memcpying to update ref counts if needed
    void memcpyed() const {
        _storage.memcpyed();
    }

    /// members for Sorter
    struct SorterDeserializeSettings {};  // unused
    void serializeForSorter(BufBuilder& buf) const;
    static Value deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&);
    int memUsageForSorter() const {
        return getApproximateSize();
    }
    Value getOwned() const {
        return *this;
    }
    void makeOwned() {}

    /// Members to support parsing/deserialization from IDL generated code.
    void serializeForIDL(StringData fieldName, BSONObjBuilder* builder) const;
    void serializeForIDL(BSONArrayBuilder* builder) const;
    static Value deserializeForIDL(const BSONElement& element);

    // Wrap a value in a BSONObj.
    BSONObj wrap(StringData newName) const;

private:
    explicit Value(const ValueStorage& storage) : _storage(storage) {}

    // May contain embedded NUL bytes, does not check the type.
    StringData getRawData() const;

    void _appendToBson(BSONObjBuilder& builder, StringData fieldName) const;

    ValueStorage _storage;
    friend class MutableValue;  // gets and sets _storage.genericRCPtr
};
MONGO_STATIC_ASSERT(sizeof(Value) == 16);

inline void swap(mongo::Value& lhs, mongo::Value& rhs) {
    lhs.swap(rhs);
}

/**
 * This class is identical to Value, but supports implicit creation from any of the types explicitly
 * supported by Value.
 */
class ImplicitValue : public Value {
public:
    template <typename T>
    requires std::is_constructible_v<Value, T>
    ImplicitValue(T&& arg) : Value(std::forward<T>(arg)) {}

    ImplicitValue(std::initializer_list<ImplicitValue> values) : Value(convertToValues(values)) {}
    ImplicitValue(std::vector<ImplicitValue> values) : Value(convertToValues(values)) {}

    template <typename T>
    ImplicitValue(std::vector<T> values) : Value(convertToValues(values)) {}

    template <typename T>
    static std::vector<Value> convertToValues(const std::vector<T>& vec) {
        std::vector<Value> values;
        values.reserve(vec.size());
        for_each(vec.begin(), vec.end(), ([&](const T& val) { values.emplace_back(val); }));
        return values;
    }

    /**
     * Converts a vector of Implicit values to a single Value object.
     */
    static Value convertToValue(const std::vector<ImplicitValue>& vec) {
        std::vector<Value> values;
        values.reserve(vec.size());
        for_each(
            vec.begin(), vec.end(), ([&](const ImplicitValue& val) { values.push_back(val); }));
        return Value(std::move(values));
    }

    /**
     * Converts a vector of Implicit values to a vector of Values.
     */
    static std::vector<Value> convertToValues(const std::vector<ImplicitValue>& list) {
        std::vector<Value> values;
        values.reserve(list.size());
        for (const ImplicitValue& val : list) {
            values.push_back(val);
        }
        return values;
    }
};
}  // namespace mongo

/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

inline size_t Value::getArrayLength() const {
    MONGO_verify(getType() == BSONType::array);
    return getArray().size();
}

inline StringData Value::getStringData() const {
    MONGO_verify(getType() == BSONType::string);
    return getRawData();
}

inline StringData Value::getRawData() const {
    return _storage.getString();
}

inline std::string Value::getString() const {
    MONGO_verify(getType() == BSONType::string);
    return std::string{_storage.getString()};
}

inline OID Value::getOid() const {
    MONGO_verify(getType() == BSONType::oid);
    return OID(_storage.oid);
}

inline bool Value::getBool() const {
    MONGO_verify(getType() == BSONType::boolean);
    return _storage.boolValue;
}

inline Date_t Value::getDate() const {
    MONGO_verify(getType() == BSONType::date);
    return Date_t::fromMillisSinceEpoch(_storage.dateValue);
}

inline Timestamp Value::getTimestamp() const {
    MONGO_verify(getType() == BSONType::timestamp);
    return Timestamp(_storage.timestampValue);
}

inline const char* Value::getRegex() const {
    MONGO_verify(getType() == BSONType::regEx);
    return _storage.getString().data();  // this is known to be NUL terminated
}
inline const char* Value::getRegexFlags() const {
    MONGO_verify(getType() == BSONType::regEx);
    const char* pattern = _storage.getString().data();  // this is known to be NUL terminated
    const char* flags = pattern + strlen(pattern) + 1;  // first byte after pattern's NUL
    dassert(flags + strlen(flags) == pattern + _storage.getString().size());
    return flags;
}

inline std::string Value::getSymbol() const {
    MONGO_verify(getType() == BSONType::symbol);
    return std::string{_storage.getString()};
}
inline std::string Value::getCode() const {
    MONGO_verify(getType() == BSONType::code);
    return std::string{_storage.getString()};
}

inline int Value::getInt() const {
    MONGO_verify(getType() == BSONType::numberInt);
    return _storage.intValue;
}

inline long long Value::getLong() const {
    BSONType type = getType();
    if (type == BSONType::numberInt)
        return _storage.intValue;

    MONGO_verify(type == BSONType::numberLong);
    return _storage.longValue;
}

inline UUID Value::getUuid() const {
    MONGO_verify(_storage.binDataType() == BinDataType::newUUID);
    auto stringData = _storage.getString();
    return UUID::fromCDR({stringData.data(), stringData.size()});
}

inline BSONBinData Value::getBinData() const {
    MONGO_verify(getType() == BSONType::binData);
    auto stringData = _storage.getString();
    return BSONBinData(stringData.data(), stringData.size(), _storage.binDataType());
}
}  // namespace mongo

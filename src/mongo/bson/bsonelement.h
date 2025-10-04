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

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>  // strlen
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace mongo {
class BSONObj;

class BSONElement;
class BSONObjBuilder;
class Timestamp;

class ExtendedCanonicalV200Generator;
class ExtendedRelaxedV200Generator;
class LegacyStrictGenerator;

/**
    BSONElement represents an "element" in a BSONObj.  So for the object { a : 3, b : "abc" },
    'a : 3' is the first element (key+value).

    The BSONElement object points into the BSONObj's data.  Thus the BSONObj must stay in scope
    for the life of the BSONElement.

    internals:
    <type><fieldName    ><value>
    -------- size() ------------
    -fieldNameSize-
    value()
    type()
*/
class BSONElement {
public:
    /**
     * Helper tag to create a BSONElement with a known field name length.
     */
    struct TrustedInitTag {
        explicit constexpr TrustedInitTag() = default;
    };

    // Declared in bsonobj_comparator_interface.h.
    class ComparatorInterface;

    /**
     * Operator overloads for relops return a DeferredComparison which can subsequently be evaluated
     * by a BSONObj::ComparatorInterface.
     */
    using DeferredComparison = BSONComparatorInterfaceBase<BSONElement>::DeferredComparison;

    /**
     * Set of rules that dictate the behavior of the comparison APIs.
     */
    using ComparisonRules = BSONComparatorInterfaceBase<BSONElement>::ComparisonRules;
    using ComparisonRulesSet = BSONComparatorInterfaceBase<BSONElement>::ComparisonRulesSet;

    /**
     * Compares two BSON elements of the same canonical type.
     *
     * Returns <0 if 'l' is less than the element 'r'.
     *         >0 if 'l' is greater than the element 'r'.
     *          0 if 'l' is equal to the element 'r'.
     */
    static int compareElements(const BSONElement& l,
                               const BSONElement& r,
                               ComparisonRulesSet rules,
                               const StringDataComparator* comparator);


    /**
     * These functions, which start with a capital letter, throw if the
     * element is not of the required type. Example:
     *
     * std::string foo = obj["foo"].String(); // std::exception if not a std::string type or DNE
     */
    std::string String() const {
        return chk(BSONType::string).str();
    }
    StringData checkAndGetStringData() const {
        return chk(BSONType::string).valueStringData();
    }
    Date_t Date() const {
        return chk(BSONType::date).date();
    }
    double Number() const {
        uassert(13118,
                str::stream() << "expected " << fieldName()
                              << " to have a numeric type, but it is a " << type(),
                isNumber());
        return number();
    }
    Decimal128 Decimal() const {
        return chk(BSONType::numberDecimal)._numberDecimal();
    }
    double Double() const {
        return chk(BSONType::numberDouble)._numberDouble();
    }
    long long Long() const {
        return chk(BSONType::numberLong)._numberLong();
    }
    int Int() const {
        return chk(BSONType::numberInt)._numberInt();
    }
    bool Bool() const {
        return chk(BSONType::boolean).boolean();
    }

    /**
     * Transform a BSON array into a vector of BSONElements.
     * If the array keys are not in sequential order or are otherwise invalid, an exception is
     * thrown.
     */
    std::vector<BSONElement> Array() const;

    mongo::OID OID() const {
        return chk(BSONType::oid).__oid();
    }

    /**
     * @return the embedded object associated with this field.
     * Note the returned object is a reference to within the parent bson object. If that
     * object is out of scope, this pointer will no longer be valid. Call getOwned() on the
     * returned BSONObj if you need your own copy.
     * throws AssertionException if the element is not of type object.
     */
    BSONObj Obj() const;

    /**
     * populate v with the value of the element.  If type does not match, throw exception.
     * useful in templates -- see also BSONObj::Vals().
     */
    void Val(Date_t& v) const {
        v = Date();
    }
    void Val(long long& v) const {
        v = Long();
    }
    void Val(Decimal128& v) const {
        v = Decimal();
    }
    void Val(bool& v) const {
        v = Bool();
    }
    void Val(BSONObj& v) const;
    void Val(mongo::OID& v) const {
        v = OID();
    }
    void Val(int& v) const {
        v = Int();
    }
    void Val(double& v) const {
        v = Double();
    }
    void Val(std::string& v) const {
        v = String();
    }

    /**
     * Use ok() to check if a value is assigned:
     * if( myObj["foo"].ok() ) ...
     */
    bool ok() const {
        return !eoo();
    }

    /**
     * True if this element has a value (ie not EOO).
     *
     * Makes it easier to check for a field's existence and use it:
     * if (auto elem = myObj["foo"]) {
     *     // Use elem
     * }
     * else {
     *     // default behavior
     * }
     */
    explicit operator bool() const {
        return ok();
    }

    std::string toString(bool includeFieldName = true, bool full = false) const;
    void toString(StringBuilder& s,
                  bool includeFieldName = true,
                  bool full = false,
                  bool redactValues = false,
                  int depth = 0) const;

    std::string jsonString(JsonStringFormat format,
                           bool includeSeparator,
                           bool includeFieldNames = true,
                           int pretty = 0,
                           size_t writeLimit = 0,
                           BSONObj* outTruncationResult = nullptr) const;

    BSONObj jsonStringBuffer(JsonStringFormat format,
                             bool includeSeparator,
                             bool includeFieldNames,
                             int pretty,
                             fmt::memory_buffer& buffer,
                             size_t writeLimit = 0) const;

    BSONObj jsonStringGenerator(ExtendedCanonicalV200Generator const& generator,
                                bool includeSeparator,
                                bool includeFieldNames,
                                int pretty,
                                fmt::memory_buffer& buffer,
                                size_t writeLimit = 0) const;
    BSONObj jsonStringGenerator(ExtendedRelaxedV200Generator const& generator,
                                bool includeSeparator,
                                bool includeFieldNames,
                                int pretty,
                                fmt::memory_buffer& buffer,
                                size_t writeLimit = 0) const;
    BSONObj jsonStringGenerator(LegacyStrictGenerator const& generator,
                                bool includeSeparator,
                                bool includeFieldNames,
                                int pretty,
                                fmt::memory_buffer& buffer,
                                size_t writeLimit = 0) const;

    operator std::string() const {
        return toString();
    }

    /**
     * Returns the type of the element
     */
    BSONType type() const {
        const signed char typeByte = ConstDataView(_data).read<signed char>();
        return static_cast<BSONType>(typeByte);
    }

    /**
     * retrieve a field within this element
     * throws exception if *this is not an embedded object
     */
    BSONElement operator[](StringData field) const;

    /**
     * See canonicalizeBSONType in bsontypes.h
     */
    int canonicalType() const {
        return canonicalizeBSONType(type());
    }

    /**
     * Indicates if it is the end-of-object element, which is present at the end of
     * every BSON object.
     */
    bool eoo() const {
        return type() == BSONType::eoo;
    }

    /**
     * Size of the element.
     */
    int size() const {
        return valuesize() + 1 + fieldNameSize();
    }

    /**
     * Wrap this element up as a singleton object.
     */
    BSONObj wrap() const;

    /**
     * Wrap this element up as a singleton object with a new name.
     */
    BSONObj wrap(StringData newName) const;

    /**
     * field name of the element.  e.g., for
     * name : "Joe"
     * "name" is the fieldname
     */
    const char* fieldName() const {
        if (eoo())
            return "";  // no fieldname for it.
        return _data + 1;
    }

    /**
     * NOTE: size includes the NULL terminator.
     */
    int fieldNameSize() const {
        return _fieldNameSize;
    }

    StringData fieldNameStringData() const {
        // if this dassert fails, someone passed bad arguments to the TrustedInit ctor.
        // TODO SERVER-104907: delete this check once the 'dassert' in TrustedInitTag ctor is
        // enabled.
        dassert(bool(eoo()) != bool(fieldNameSize()));
        return StringData(fieldName(), _fieldNameSize ? _fieldNameSize - 1 : 0);
    }

    /**
     * raw data of the element's value (so be careful).
     */
    const char* value() const {
        return _data + fieldNameSize() + 1;
    }
    /**
     * size in bytes of the element's value (when applicable).
     */
    int valuesize() const {
        auto type = static_cast<uint8_t>(*_data);
        uint32_t mask = 1u << (type & 0x1fu);
        int32_t size = kFixedSizes[type];
        if (mask & kVariableSizeMask)  // These types use a 32-bit int to store their size
            size += ConstDataView(value()).read<LittleEndian<int32_t>>();
        if (MONGO_unlikely(!size))  // 0 means we have a regex, minkey, maxkey, or invalid element
            return computeRegexSize(_data, fieldNameSize()) - fieldNameSize() - 1;
        return size - 1;
    }

    bool isBoolean() const {
        return type() == BSONType::boolean;
    }

    /**
     * @return value of a boolean element.
     * You must assure element is a boolean before
     * calling.
     */
    bool boolean() const {
        return *value() ? true : false;
    }

    bool booleanSafe() const {
        return isBoolean() && boolean();
    }

    /**
     * Retrieve a java style date value from the element.
     * Ensure element is of type Date before calling.
     * @see Bool(), trueValue()
     */
    Date_t date() const {
        return Date_t::fromMillisSinceEpoch(ConstDataView(value()).read<LittleEndian<long long>>());
    }

    /**
     * Convert the value to boolean, regardless of its type, in a javascript-like fashion
     * (i.e., treats zero and null and eoo as false).
     */
    bool trueValue() const;

    /**
     * True if element is of a numeric type.
     */
    bool isNumber() const;

    /**
     * True if element is a NaN double or decimal.
     */
    bool isNaN() const;

    /**
     * Return double value for this field. MUST be NumberDouble type.
     */
    double _numberDouble() const {
        return ConstDataView(value()).read<LittleEndian<double>>();
    }

    /**
     * Return int value for this field. MUST be NumberInt type.
     */
    int _numberInt() const {
        return ConstDataView(value()).read<LittleEndian<int>>();
    }

    /**
     * Return decimal128 value for this field. MUST be NumberDecimal type.
     */
    Decimal128 _numberDecimal() const {
        uint64_t low = ConstDataView(value()).read<LittleEndian<long long>>();
        uint64_t high = ConstDataView(value() + sizeof(long long)).read<LittleEndian<long long>>();
        return Decimal128(Decimal128::Value({low, high}));
    }

    /**
     * Return long long value for this field. MUST be NumberLong type.
     */
    long long _numberLong() const {
        return ConstDataView(value()).read<LittleEndian<long long>>();
    }

    /**
     * Retrieves the value of this element as a 32 bit integer. If the BSON type is non-numeric,
     * returns zero. If the element holds a double, truncates the fractional part.
     *
     * Results in undefined behavior if called on a double that is NaN, +/-infinity, or too
     * large/small to be represented as an int.  Use 'safeNumberLong()' to safely convert an
     * arbitrary BSON element to an integer without risk of UB.
     */
    int numberInt() const;

    /**
     * Like numberInt() but with well-defined behavior for doubles that
     * are NaNs, or too large/small to be represented as int.
     * NaNs -> 0
     * very large doubles -> INT_MAX
     * very small doubles -> INT_MIN
     */
    int safeNumberInt() const;

    /**
     * Retrieves the value of this element as a 64 bit integer. If the BSON type is non-numeric,
     * returns zero. If the element holds a double, truncates the fractional part.
     *
     * Results in undefined behavior if called on a double that is NaN, +/-infinity, or too
     * large/small to be repsented as a long. Use 'safeNumberLong()' to safely convert an arbitrary
     * BSON element to an integer without risk of UB.
     */
    long long numberLong() const;

    /**
     * Like numberLong() but with well-defined behavior for doubles that
     * are NaNs, or too large/small to be represented as long longs.
     * NaNs -> 0
     * very large doubles -> LLONG_MAX
     * very small doubles -> LLONG_MIN
     */
    long long safeNumberLong() const;

    /**
     * This safeNumberLongForHash() function does the same thing as safeNumberLong, but it
     * preserves edge-case behavior from older versions.
     */
    long long safeNumberLongForHash() const;

    /**
     * Convert a numeric field to long long, and uassert the conversion is exact.
     */
    long long exactNumberLong() const;

    /**
     * Parses a BSONElement of any numeric type into a non-negative long long, failing if the value
     * is any of the following:
     *
     * - NaN.
     * - Negative.
     * - A floating point number which is not integral.
     * - Too large to fit within a 64-bit signed integer.
     */
    StatusWith<long long> parseIntegerElementToNonNegativeLong() const;

    /**
     * Parses a BSONElement of any numeric type into a long long, failing if the value
     * is any of the following:
     *
     * - NaN.
     * - A floating point number which is not integral.
     * - Too large in the positive or negative direction to fit within a 64-bit signed integer.
     */
    StatusWith<long long> parseIntegerElementToLong() const;

    /**
     * Parses a BSONElement of any numeric type into a non-negative int, failing if the value
     * is any of the following:
     *
     * - NaN
     * - Negative
     * - a non-integral number
     * - too large in the positive or negative direction to fit in an int
     */
    StatusWith<int> parseIntegerElementToNonNegativeInt() const;

    /**
     * Parses a BSONElement of any numeric type into an integer, failing if the value is:
     *
     * - NaN
     * - a non-integral number
     * - too large in the positive or negative direction to fit in an int
     */
    StatusWith<int> parseIntegerElementToInt() const;

    /**
     * Retrieve decimal value for the element safely.
     */
    Decimal128 numberDecimal() const;

    /**
     * Retrieve the numeric value of the element.  If not of a numeric type, returns 0.
     * Note: casts to double, data loss may occur with large (>52 bit) NumberLong values.
     */
    double numberDouble() const;
    /**
     * Retrieve the numeric value of the element.  If not of a numeric type, returns 0.
     * Note: casts to double, data loss may occur with large (>52 bit) NumberLong values.
     */
    double number() const {
        return numberDouble();
    }

    /**
     * Like numberDouble() but with well-defined behavior for doubles that
     * are NaNs, or too large/small to be represented as doubles.
     * NaNs -> 0
     * very large decimals -> DOUBLE_MAX
     * very small decimals -> DOUBLE_MIN
     */
    double safeNumberDouble() const;

    /**
     * Retrieve the object ID stored in the object.
     * You must ensure the element is of type jstOID first.
     */
    mongo::OID __oid() const {
        return OID::from(value());
    }

    /**
     * True if element is null.
     */
    bool isNull() const {
        return type() == BSONType::null;
    }

    /**
     * Size of a BSON String element.
     * Requires that type() == BSONType::string.
     * @return String size including its null-termination.
     */
    int valuestrsize() const {
        return ConstDataView(value()).read<LittleEndian<int>>();
    }

    /**
     * for objects the size *includes* the size of the size field
     */
    size_t objsize() const {
        return ConstDataView(value()).read<LittleEndian<uint32_t>>();
    }

    /**
     * Get a string's value. Returns a valid empty string if
     * `type() != BSONType::string`.
     */
    StringData valueStringDataSafe() const {
        return type() == BSONType::string ? StringData(valuestr(), valuestrsize() - 1)
                                          : StringData();
    }

    /**
     * Like valueStringDataSafe, but returns std::string.
     */
    std::string str() const {
        return std::string{valueStringDataSafe()};
    }

    /**
     * Returns a StringData pointing into this element's data.  Does not validate that the
     * element is actually of type String.
     */
    StringData valueStringData() const {
        return StringData(valuestr(), valuestrsize() - 1);
    }

    /**
     * Get javascript code of a CodeWScope data element.
     */
    const char* codeWScopeCode() const {
        massert(16177, "not codeWScope", type() == BSONType::codeWScope);
        return value() + 4 + 4;  // two ints precede code (see BSON spec)
    }

    /**
     * Get length of the code part of the CodeWScope object
     * This INCLUDES the null char at the end
     */
    int codeWScopeCodeLen() const {
        massert(16178, "not codeWScope", type() == BSONType::codeWScope);
        return ConstDataView(value() + 4).read<LittleEndian<int>>();
    }

    /**
     * Get the scope SavedContext of a CodeWScope data element.
     */
    const char* codeWScopeScopeData() const {
        return codeWScopeCode() + codeWScopeCodeLen();
    }

    /**
     * Get the embedded object this element holds.
     */
    BSONObj embeddedObject() const;

    /**
     * uasserts if not an object
     */
    BSONObj embeddedObjectUserCheck() const;

    BSONObj codeWScopeObject() const;

    /**
     * Get raw binary data.  Element must be of type BinData. Doesn't handle type 2 specially
     */
    const char* binData(int& len) const {
        // BinData: <int len> <byte subtype> <byte[len] data>
        MONGO_verify(type() == BSONType::binData);
        len = valuestrsize();
        return value() + 5;
    }
    /**
     * Get binary data.  Element must be of type BinData. Handles type 2
     */
    const char* binDataClean(int& len) const {
        // BinData: <int len> <byte subtype> <byte[len] data>
        if (binDataType() != ByteArrayDeprecated) {
            return binData(len);
        } else {
            // Skip extra size
            len = valuestrsize() - 4;
            return value() + 5 + 4;
        }
    }

    static BinDataType binDataType(const char* raw, size_t length) {
        // BinData: <int len> <byte subtype> <byte[len] data>
        MONGO_verify(length >= 5);
        unsigned char c = raw[4];
        return static_cast<BinDataType>(c);
    }

    BinDataType binDataType() const {
        // BinData: <int len> <byte subtype> <byte[len] data>
        MONGO_verify(type() == BSONType::binData);
        unsigned char c = (value() + 4)[0];
        return static_cast<BinDataType>(c);
    }

    std::vector<uint8_t> _binDataVector() const {
        auto first = reinterpret_cast<const uint8_t*>(value()) + 5;
        auto last = first + valuestrsize();
        if (binDataType() == ByteArrayDeprecated)
            first += std::min<size_t>(4, last - first);  // skip extra int32 size.
        return {first, last};
    }

    /**
     * Retrieve the regex std::string for a Regex element
     */
    const char* regex() const {
        MONGO_verify(type() == BSONType::regEx);
        return value();
    }

    /**
     * Retrieve the regex flags (options) for a Regex element
     */
    const char* regexFlags() const {
        const char* p = regex();
        return p + strlen(p) + 1;
    }

    //
    // Comparison API.
    //
    // BSONElement instances can be compared via a raw bytewise comparison or a logical comparison.
    //
    // Logical comparison can be done either using woCompare() or with operator overloads. Most
    // callers should prefer operator overloads. Note that the operator overloads return a
    // DeferredComparison, which must subsequently be evaluated by a
    // BSONElement::ComparatorInterface. See bsonelement_comparator_interface.h for details.
    //

    /**
     * Compares the raw bytes of the two BSONElements, including the field names. This will treat
     * different types (e.g. integers and doubles) as distinct values, even if they have the same
     * field name and bit pattern in the value portion of the BSON element.
     */
    bool binaryEqual(const BSONElement& rhs) const;

    /**
     * Compares the raw bytes of the two BSONElements, excluding the field names. This will treat
     * different types (e.g integers and doubles) as distinct values, even if they have the same bit
     * pattern in the value portion of the BSON element.
     */
    bool binaryEqualValues(const BSONElement& rhs) const;

    /**
     * Compares two BSON Elements using the rules specified by 'rules' and the 'comparator' for
     * string comparisons.
     *
     * Returns <0 if 'this' is less than 'elem'.
     *         >0 if 'this' is greater than 'elem'.
     *          0 if 'this' is equal to 'elem'.
     */
    int woCompare(const BSONElement& elem,
                  ComparisonRulesSet rules = ComparisonRules::kConsiderFieldName,
                  const StringDataComparator* comparator = nullptr) const;

    DeferredComparison operator<(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kLT, *this, other);
    }

    DeferredComparison operator<=(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kLTE, *this, other);
    }

    DeferredComparison operator>(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kGT, *this, other);
    }

    DeferredComparison operator>=(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kGTE, *this, other);
    }

    DeferredComparison operator==(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kEQ, *this, other);
    }

    DeferredComparison operator!=(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kNE, *this, other);
    }

    const char* rawdata() const {
        return _data;
    }

    /**
     * True if this element may contain subobjects.
     */
    bool mayEncapsulate() const {
        switch (type()) {
            case BSONType::object:
            case BSONType::array:
            case BSONType::codeWScope:
                return true;
            default:
                return false;
        }
    }

    /**
     * True if this element can be a BSONObj
     */
    bool isABSONObj() const {
        switch (type()) {
            case BSONType::object:
            case BSONType::array:
                return true;
            default:
                return false;
        }
    }

    /**
     * Returns BSON types Timestamp and Date as Timestamp.
     *
     * This can be dangerous if the result is used for comparisons as Timestamp is an unsigned type
     * where Date is signed. Instead, consider using date() when a timestamp before the unix epoch
     * is possible.
     */
    Timestamp timestamp() const {
        if (type() == BSONType::date || type() == BSONType::timestamp) {
            return Timestamp(ConstDataView(value()).read<LittleEndian<unsigned long long>>().value);
        }
        return Timestamp();
    }

    bool isBinData(BinDataType bdt) const {
        return (type() == BSONType::binData) && (binDataType() == bdt);
    }

    std::array<unsigned char, 16> uuid() const {
        int len = 0;
        const char* data = nullptr;
        if (isBinData(BinDataType::newUUID)) {
            data = binData(len);
        }
        uassert(ErrorCodes::InvalidUUID,
                "uuid must be a 16-byte binary field with UUID (4) subtype",
                len == 16);
        std::array<unsigned char, 16> result;
        memcpy(&result, data, len);
        return result;
    }

    std::array<unsigned char, 16> md5() const {
        int len = 0;
        const char* data = nullptr;
        if (isBinData(BinDataType::MD5Type)) {
            data = binData(len);
        }
        uassert(40437, "md5 must be a 16-byte binary field with MD5 (5) subtype", len == 16);
        std::array<unsigned char, 16> result;
        memcpy(&result, data, len);
        return result;
    }


    Date_t timestampTime() const {
        unsigned long long t = ConstDataView(value() + 4).read<LittleEndian<unsigned int>>();
        return Date_t::fromMillisSinceEpoch(t * 1000);
    }
    unsigned int timestampInc() const {
        return ConstDataView(value()).read<LittleEndian<unsigned int>>();
    }

    unsigned long long timestampValue() const {
        return ConstDataView(value()).read<LittleEndian<unsigned long long>>();
    }

    const char* dbrefNS() const {
        uassert(10063, "not a dbref", type() == BSONType::dbRef);
        return value() + 4;
    }

    mongo::OID dbrefOID() const {
        uassert(10064, "not a dbref", type() == BSONType::dbRef);
        const char* start = value();
        start += 4 + ConstDataView(start).read<LittleEndian<int>>();
        return mongo::OID::from(start);
    }

    constexpr BSONElement() = default;

    explicit BSONElement(const char* d) : _data(d) {
        // While we should skip the type, and add 1 for the terminating null byte, just include
        // the type byte in the strlen loop: the extra byte cancels out. As an extra bonus, this
        // also handles the EOO case, where the type byte is 0.
        while (*d)
            ++d;
        _fieldNameSize = d - _data;
    }

    /**
     * Construct a BSONElement where you already know the length of the name.
     * The fieldNameSize must match 'BSONElement(d).fieldNameSize()'. In particular,
     * - it includes the NUL terminator
     * - 'fieldNameSize == 0' iff *d == EOO
     */
    constexpr BSONElement(const char* d, int fieldNameSize, TrustedInitTag)
        : _data(d), _fieldNameSize(fieldNameSize) {
        // TODO SERVER-104907: enable validation here once all callers have been adjusted/fixed.
        // dassert((*d == stdx::to_underlying(BSONType::eoo)) != bool(fieldNameSize));
    }

    std::string _asCode() const;

    bool coerce(std::string* out) const;

    /**
     * Coerces the value to an int. If the value type is NumberDouble, the value is rounded to
     * a closest integer towards zero. If the value type is NumberDecimal, the value is rounded to a
     * closest integer, but ties are rounded to an even integer. Returns false, if the value cannot
     * be coerced.
     */
    bool coerce(int* out) const;

    /**
     * Coerces the value to a long long. If the value type is NumberDouble, the value is rounded to
     * a closest integer towards zero. If the value type is NumberDecimal, the value is rounded to a
     * closest integer, but ties are rounded to an even integer. Returns false, if the value cannot
     * be coerced.
     */
    bool coerce(long long* out) const;
    bool coerce(double* out) const;
    bool coerce(bool* out) const;
    bool coerce(Decimal128* out) const;
    bool coerce(std::vector<std::string>* out) const;

    template <typename T>
    Status tryCoerce(T* out) const;

    /**
     * Constant double representation of 2^63, the smallest value that will overflow a long long.
     *
     * It is not safe to obtain this value by casting std::numeric_limits<long long>::max() to
     * double, because the conversion loses precision, and the C++ standard leaves it up to the
     * implementation to decide whether to round up to 2^63 or round down to the next representable
     * value (2^63 - 2^10).
     */
    static const double kLongLongMaxPlusOneAsDouble;

    /**
     * Constant 'long long' representation of 2^53 (and -2^53). This is the largest (and smallest)
     * 'long long' such that all 'long long's between the two can be safely represented as a double
     * without losing precision.
     */
    static const long long kLargestSafeLongLongAsDouble;
    static const long long kSmallestSafeLongLongAsDouble;

private:
    // This needs to be 2 elements because we check the strlen of data + 1 and GCC sees that as
    // accessing beyond the end of a constant string, even though we always check whether the
    // element is an eoo.
    static constexpr const char kEooElement[2] = {'\0', '\0'};

    /**
     *  The kFixedSizes table provides the fixed size of each element type, including the type byte,
     *  but excluding the field name and its terminating 0 byte, and excluding the variable sized
     *  part for objects, arrays, strings, etc. Elements that are 0 may be regex/minkey/maxkey
     *  elements, or elements with invalid types. The table is extended to 256 bytes to make it more
     *  likely that we will error on such invalid type bytes in the case of memory corruption.
     *  The alignas clause ensures the first part of the table fits on a single cache line, which
     *  avoids performance changes due to memory layout.
     */
    static constexpr uint8_t kFixedSizes alignas(32)[256] = {
        1,                        // 0x00 EOO
        9,                        // 0x01 NumberDouble
        5,                        // 0x02 String
        1,                        // 0x03 Object
        1,                        // 0x04 Array
        6,                        // 0x05 BinData
        1,                        // 0x06 Undefined
        13,                       // 0x07 ObjectID
        2,                        // 0x08 Bool
        9,                        // 0x09 Date
        1,                        // 0x0a Null
        0,                        // 0x0b Regex
        17,                       // 0x0c DBRef
        5,                        // 0x0d Code
        5,                        // 0x0e Symbol
        1,                        // 0x0f CodeWScope
        5,                        // 0x10 NumberInt
        9,                        // 0x11 Timestamp
        9,                        // 0x12 NumberLong
        17,                       // 0x13 NumberDecimal
        0,  0, 0, 0,              // 0x14 - 0x17 (reserved)
        0,  0, 0, 0, 0, 0, 0, 0,  // 0x18 - 0x1f (reserved)
    };
    MONGO_STATIC_ASSERT(sizeof(kFixedSizes) == 256);

    /**
     * The kVariableSizeMask table provides a mask for the types that have a variable size. The mask
     * is 1 << type, so that we can use it to check whether a type is variable size with a single
     * bit test.
     */
    static constexpr uint32_t kVariableSizeMask =  // equal to 0xf03cu (61500)
        (1u << stdx::to_underlying(BSONType::string)) |
        (1u << stdx::to_underlying(BSONType::object)) |
        (1u << stdx::to_underlying(BSONType::array)) |
        (1u << stdx::to_underlying(BSONType::binData)) |
        (1u << stdx::to_underlying(BSONType::dbRef)) | (1u << stdx::to_underlying(BSONType::code)) |
        (1u << stdx::to_underlying(BSONType::symbol)) |
        (1u << stdx::to_underlying(BSONType::codeWScope));

    /**
     * This is an out-of-line helper only for use as the slow path of valuesize()!
     *
     * Computes the size of the encoding of a Regex, MinKey or MaxKey. Throws if the element is
     * not any of these. This function is suitable as a fallback after other known BSON types are
     * handled, as it will try to dump extra diagnostic information in case of memory corruption.
     */
    static int computeRegexSize(const char* data, int fieldNameSize);

    /**
     * This is to enable structured bindings for BSONElement, it should not be used explicitly.
     * When used in a structed binding, BSONElement behaves as-if it is a
     * std::pair<StringData, BSONElement>.
     *
     * Example:
     *   for (auto [name, elem] : someBsonObj) {...}
     */
    template <size_t I>
    friend auto get(const BSONElement& elem) {
        static_assert(I <= 1);
        if constexpr (I == 0) {
            return elem.fieldNameStringData();
        } else if constexpr (I == 1) {
            return elem;
        }
    }

    /**
     * Get a string's value. Also gives you start of the real data for an embedded object.
     * You must assure data is of an appropriate type first, like the type check in
     * valueStringDataSafe(). You should use the string's size when performing any operations
     * on the data to disambiguate between potential embedded null's and the terminating null.
     * This function is only used in limited forms internally. Not to be exposed publicly.
     * If a char* is desired use valueStringDataSafe().data().
     */
    const char* valuestr() const {
        return value() + 4;
    }

    template <typename Generator>
    BSONObj _jsonStringGenerator(const Generator& g,
                                 bool includeSeparator,
                                 bool includeFieldNames,
                                 int pretty,
                                 fmt::memory_buffer& buffer,
                                 size_t writeLimit) const;

    friend class BSONObjIterator;
    friend class BSONObjStlIterator;
    friend class BSONObj;
    const BSONElement& chk(BSONType t) const {
        if (t != type()) {
            StringBuilder ss;
            if (eoo())
                ss << "field not found, expected type " << t;
            else
                ss << "wrong type for field (" << fieldName() << ") " << type() << " != " << t;
            uasserted(13111, ss.str());
        }
        return *this;
    }

    const char* _data = kEooElement;
    int _fieldNameSize = 0;  // internal size includes null terminator, 0 for EOO
};

inline bool BSONElement::trueValue() const {
    // NOTE Behavior changes must be replicated in Value::coerceToBool().
    switch (type()) {
        case BSONType::numberLong:
            return _numberLong() != 0;
        case BSONType::numberDouble:
            return _numberDouble() != 0;
        case BSONType::numberDecimal:
            return _numberDecimal().isNotEqual(Decimal128(0));
        case BSONType::numberInt:
            return _numberInt() != 0;
        case BSONType::boolean:
            return boolean();
        case BSONType::eoo:
        case BSONType::null:
        case BSONType::undefined:
            return false;
        default:
            return true;
    }
}

/**
 * @return true if element is of a numeric type.
 */
inline bool BSONElement::isNumber() const {
    switch (type()) {
        case BSONType::numberLong:
        case BSONType::numberDouble:
        case BSONType::numberDecimal:
        case BSONType::numberInt:
            return true;
        default:
            return false;
    }
}

inline bool BSONElement::isNaN() const {
    switch (type()) {
        case BSONType::numberDouble: {
            double d = _numberDouble();
            return std::isnan(d);
        }
        case BSONType::numberDecimal: {
            Decimal128 d = _numberDecimal();
            return d.isNaN();
        }
        default:
            return false;
    }
}

inline Decimal128 BSONElement::numberDecimal() const {
    switch (type()) {
        case BSONType::numberDouble:
            return Decimal128(_numberDouble());
        case BSONType::numberInt:
            return Decimal128(_numberInt());
        case BSONType::numberLong:
            return Decimal128(static_cast<int64_t>(_numberLong()));
        case BSONType::numberDecimal:
            return _numberDecimal();
        default:
            return Decimal128::kNormalizedZero;
    }
}

inline double BSONElement::numberDouble() const {
    switch (type()) {
        case BSONType::numberDouble:
            return _numberDouble();
        case BSONType::numberInt:
            return _numberInt();
        case BSONType::numberLong:
            return _numberLong();
        case BSONType::numberDecimal:
            return _numberDecimal().toDouble();
        default:
            return 0;
    }
}

inline double BSONElement::safeNumberDouble() const {
    switch (type()) {
        case BSONType::numberDouble: {
            double d = _numberDouble();
            if (std::isnan(d)) {
                return 0;
            }
            return d;
        }
        case BSONType::numberInt: {
            return _numberInt();
        }
        case BSONType::numberLong: {
            long long d = _numberLong();
            if (d > 0 && d > kLargestSafeLongLongAsDouble) {
                return static_cast<double>(kLargestSafeLongLongAsDouble);
            }
            if (d < 0 && d < kSmallestSafeLongLongAsDouble) {
                return static_cast<double>(kSmallestSafeLongLongAsDouble);
            }
            return d;
        }
        case BSONType::numberDecimal: {
            Decimal128 d = _numberDecimal();
            if (d.isNaN()) {
                return 0;
            }
            if (d.isGreater(Decimal128(std::numeric_limits<double>::max()))) {
                return std::numeric_limits<double>::max();
            }
            if (d.isLess(Decimal128(std::numeric_limits<double>::min()))) {
                return std::numeric_limits<double>::min();
            }
            return _numberDecimal().toDouble();
        }
        default:
            return 0;
    }
}

inline int BSONElement::numberInt() const {
    switch (type()) {
        case BSONType::numberDouble:
            return (int)_numberDouble();
        case BSONType::numberInt:
            return _numberInt();
        case BSONType::numberLong:
            return (int)_numberLong();
        case BSONType::numberDecimal:
            return _numberDecimal().toInt();
        default:
            return 0;
    }
}

inline int BSONElement::safeNumberInt() const {
    return static_cast<int>(std::clamp<long long>(
        safeNumberLong(), std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}

inline long long BSONElement::numberLong() const {
    switch (type()) {
        case BSONType::numberDouble:
            return (long long)_numberDouble();
        case BSONType::numberInt:
            return _numberInt();
        case BSONType::numberLong:
            return _numberLong();
        case BSONType::numberDecimal:
            return _numberDecimal().toLong();
        default:
            return 0;
    }
}

/**
 * Like numberLong() but with well-defined behavior for doubles and decimals that
 * are NaNs, or too large/small to be represented as long longs.
 * NaNs -> 0
 * very large values -> LLONG_MAX
 * very small values -> LLONG_MIN
 */
inline long long BSONElement::safeNumberLong() const {
    switch (type()) {
        case BSONType::numberDouble: {
            double d = numberDouble();
            if (std::isnan(d)) {
                return 0;
            }
            if (!(d < kLongLongMaxPlusOneAsDouble)) {
                return std::numeric_limits<long long>::max();
            }
            if (d < std::numeric_limits<long long>::min()) {
                return std::numeric_limits<long long>::min();
            }
            return numberLong();
        }
        case BSONType::numberDecimal: {
            Decimal128 d = numberDecimal();
            if (d.isNaN()) {
                return 0;
            }
            if (d.isGreater(Decimal128(std::numeric_limits<int64_t>::max()))) {
                return static_cast<long long>(std::numeric_limits<int64_t>::max());
            }
            if (d.isLess(Decimal128(std::numeric_limits<int64_t>::min()))) {
                return static_cast<long long>(std::numeric_limits<int64_t>::min());
            }
            return numberLong();
        }
        default:
            return numberLong();
    }
}

/**
 * Attempt to coerce the BSONElement to a primitive type. For integral targets, we do additional
 * checking that the source number is a finite real number and fits within the target type after
 * rounding to the closest integer towards zero. Note that for NumberDecimal types the real number
 * rounding behavior of this method is different from one employed by 'coerce'.
 */
template <typename T>
Status BSONElement::tryCoerce(T* out) const {
    if constexpr (std::is_integral<T>::value && !std::is_same<bool, T>::value) {
        long long val;
        if (type() == BSONType::numberDouble) {
            double d = numberDouble();
            if (!std::isfinite(d)) {
                return {ErrorCodes::BadValue, "Unable to coerce NaN/Inf to integral type"};
            }
            constexpr bool sameMax =
                std::numeric_limits<T>::max() == std::numeric_limits<long long>::max();
            if ((!sameMax && d > static_cast<double>(std::numeric_limits<T>::max())) ||
                (sameMax && d >= static_cast<double>(std::numeric_limits<T>::max())) ||
                (d < std::numeric_limits<T>::lowest())) {
                return {ErrorCodes::BadValue, "Out of bounds coercing to integral value"};
            }
            val = static_cast<long long>(d);
        } else if (type() == BSONType::numberDecimal) {
            Decimal128 d = numberDecimal();
            if (!d.isFinite()) {
                return {ErrorCodes::BadValue, "Unable to coerce NaN/Inf to integral type"};
            }
            d = d.round(Decimal128::RoundingMode::kRoundTowardZero);
            if (d.isGreater(Decimal128(std::numeric_limits<T>::max())) ||
                d.isLess(Decimal128(std::numeric_limits<T>::lowest()))) {
                return {ErrorCodes::BadValue, "Out of bounds coercing to integral value"};
            }
            uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
            val = d.toLongExact(&signalingFlags);
            tassert(5732103,
                    "decimal128 number exact conversion to long failed",
                    Decimal128::SignalingFlag::kNoFlag == signalingFlags);
        } else if (type() == BSONType::boolean) {
            *out = Bool();
            return Status::OK();
        } else if (!coerce(&val)) {
            return {ErrorCodes::BadValue, "Unable to coerce value to integral type"};
        }

        if (std::is_same<long long, T>::value) {
            *out = val;
            return Status::OK();
        }

        if ((val > std::numeric_limits<T>::max()) || (val < std::numeric_limits<T>::lowest())) {
            return {ErrorCodes::BadValue, "Out of bounds coercing to integral value"};
        }

        *out = static_cast<T>(val);
        return Status::OK();
    }

    if (!coerce(out)) {
        return {ErrorCodes::BadValue, "Unable to coerce value to correct type"};
    }

    return Status::OK();
}
/**
 * This safeNumberLongForHash() function does the same thing as safeNumberLong, but it preserves
 * edge-case behavior from older versions. It's provided for use by hash functions that need to
 * maintain compatibility with older versions. Don't make any changes to safeNumberLong() without
 * ensuring that this function (which is implemented in terms of safeNumberLong()) has exactly the
 * same behavior.
 *
 * Historically, safeNumberLong() used a check that would consider 2^63 to be safe to cast to
 * int64_t, but that cast actually overflows. On most platforms, the undefined cast of 2^63 to
 * int64_t would roll over to -2^63, and that's the behavior we preserve here explicitly.
 *
 * The new safeNumberLong() function uses a tight bound, allowing it to correctly clamp double 2^63
 * to the max 64-bit int (2^63 - 1).
 */
inline long long BSONElement::safeNumberLongForHash() const {
    // Rather than relying on the undefined overflow conversion, we maintain compatibility by
    // explicitly checking for a 2^63 double value and returning -2^63.
    if (type() == BSONType::numberDouble &&
        numberDouble() == BSONElement::kLongLongMaxPlusOneAsDouble) {
        return std::numeric_limits<long long>::lowest();
    } else {
        return safeNumberLong();
    }
}

inline long long BSONElement::exactNumberLong() const {
    return uassertStatusOK(parseIntegerElementToLong());
}

}  // namespace mongo

// These template specializations in namespace std are required in order to support structured
// bindings using the "tuple protocol".
namespace std {
template <>
struct tuple_size<mongo::BSONElement> : std::integral_constant<size_t, 2> {};
template <size_t I>
struct tuple_element<I, mongo::BSONElement>
    : std::tuple_element<I, std::pair<mongo::StringData, mongo::BSONElement>> {};
}  // namespace std

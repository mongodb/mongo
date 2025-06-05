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

#include "mongo/base/counter.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <iosfwd>
#include <limits>
#include <type_traits>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

class BSONArrayBuilder;
class BSONElement;
class BSONElementCmpWithoutField;
class BSONObj;
class BSONObjBuilder;
class BSONObjIterator;
class Ordering;
struct BSONArray;  // empty subclass of BSONObj useful for overloading

extern const BSONObj kMaxBSONKey;
extern const BSONObj kMinBSONKey;

/**
 *  The complete list of valid BSON types. See also bsonspec.org.
 */
enum class BSONType : int {
    /** smaller than all other types */
    minKey = -1,
    /** end of object */
    eoo = 0,
    /** double precision floating point value */
    numberDouble = 1,
    /** character string, stored in utf8 */
    string = 2,
    /** an embedded object */
    object = 3,
    /** an embedded array */
    array = 4,
    /** binary data */
    binData = 5,
    /** (Deprecated) Undefined type */
    undefined = 6,
    /** ObjectId */
    oid = 7,
    /** boolean type */
    boolean = 8,
    /** date type */
    date = 9,
    /** null type */
    null = 10,
    /** regular expression, a pattern with options */
    regEx = 11,
    /** (Deprecated) */
    dbRef = 12,
    /** code type */
    code = 13,
    /** (Deprecated) a programming language (e.g., Python) symbol */
    symbol = 14,
    /** (Deprecated) javascript code that can execute on the database server, with SavedContext */
    codeWScope = 15,
    /** 32 bit signed integer */
    numberInt = 16,
    /** Two 32 bit signed integers */
    timestamp = 17,
    /** 64 bit integer */
    numberLong = 18,
    /** 128 bit decimal */
    numberDecimal = 19,
    /** max type that is not MaxKey */
    jsTypeMax = 19,
    /** larger than all other types */
    maxKey = 127
};

inline auto format_as(BSONType t) {
    return fmt::underlying(t);
}

/**
 * Maps from the set of type aliases accepted by the $type query operator to the corresponding BSON
 * types. Excludes "number", since this alias maps to a set of BSON types.
 */
boost::optional<BSONType> findBSONTypeAlias(StringData key);

/**
 * returns the name of the argument's type
 */
const char* typeName(BSONType type);

/**
 * Reverse mapping of typeName(). Throws an exception with error code BadValue when passed in
 * invalid type name.
 */
BSONType typeFromName(StringData name);

/**
 * Prints the name of the argument's type to the given stream.
 */
std::ostream& operator<<(std::ostream& stream, BSONType type);

/**
 * Returns whether or not 'type' can be converted to a valid BSONType.
 */
bool isValidBSONType(int type);

/**
 * IDL callback validator
 */
Status isValidBSONTypeName(StringData typeName);


inline bool isNumericBSONType(BSONType type) {
    switch (type) {
        case BSONType::numberDouble:
        case BSONType::numberInt:
        case BSONType::numberLong:
        case BSONType::numberDecimal:
            return true;
        default:
            return false;
    }
}

/**
 * Given a type, returns whether or not that type has a variable width.
 **/
inline bool isVariableWidthType(BSONType type) {
    switch (type) {
        case BSONType::array:
        case BSONType::binData:
        case BSONType::code:
        case BSONType::codeWScope:
        case BSONType::dbRef:
        case BSONType::object:
        case BSONType::regEx:
        case BSONType::string:
        case BSONType::symbol:
            return true;
        case BSONType::boolean:
        case BSONType::timestamp:
        case BSONType::date:
        case BSONType::eoo:
        case BSONType::null:
        case BSONType::oid:
        case BSONType::maxKey:
        case BSONType::minKey:
        case BSONType::numberDecimal:
        case BSONType::numberDouble:
        case BSONType::numberInt:
        case BSONType::numberLong:
        case BSONType::undefined:
            return false;
        default:
            MONGO_UNREACHABLE;
    }
}

/* subtypes of BinData.
   bdtCustom and above are ones that the JS compiler understands, but are
   opaque to the database.
*/
enum BinDataType {
    BinDataGeneral = 0,
    Function = 1,
    ByteArrayDeprecated = 2, /* use BinGeneral instead */
    bdtUUID = 3,             /* deprecated */
    newUUID = 4,             /* language-independent UUID format across all drivers */
    MD5Type = 5,
    Encrypt = 6,   /* encryption placeholder or encrypted data */
    Column = 7,    /* compressed column */
    Sensitive = 8, /* data that should be redacted and protected from unnecessary exposure */
    Vector = 9,    /* A denser format of an array of numbers representing a vector */
    bdtCustom = 128
};

inline auto format_as(BinDataType type) {
    return fmt::underlying(type);
}

/**
 * Return the name of the BinData Type.
 */
const char* typeName(BinDataType type);

/**
 * Returns whether or not 'type' can be converted to a valid BinDataType.
 */
bool isValidBinDataType(int type);

/** Returns a number for where a given type falls in the sort order.
 *  Elements with the same return value should be compared for value equality.
 *  The return value is not a BSONType and should not be treated as one.
 *  Note: if the order changes, indexes have to be re-built or there can be corruption
 */
inline constexpr std::int8_t canonicalizeBSONTypeUnsafeLookup(BSONType type) {
    // This switch statement gets compiled down to a lookup table in GCC >= 8.5
    // To achieve this there must be NO exceptions thrown nor functions called as
    // otherwise it would break the optimization.
    // Additionally, it must also contain a default case in order to fully build
    // out the lookup table as it won't generate it as such without it.
    switch (type) {
        case BSONType::minKey:
            return stdx::to_underlying(BSONType::minKey);
        case BSONType::maxKey:
            return stdx::to_underlying(BSONType::maxKey);
        case BSONType::eoo:
        case BSONType::undefined:
            return 0;
        case BSONType::null:
            return 5;
        case BSONType::numberDecimal:
        case BSONType::numberDouble:
        case BSONType::numberInt:
        case BSONType::numberLong:
            return 10;
        case BSONType::string:
        case BSONType::symbol:
            return 15;
        case BSONType::object:
            return 20;
        case BSONType::array:
            return 25;
        case BSONType::binData:
            return 30;
        case BSONType::oid:
            return 35;
        case BSONType::boolean:
            return 40;
        case BSONType::date:
            return 45;
        case BSONType::timestamp:
            return 47;
        case BSONType::regEx:
            return 50;
        case BSONType::dbRef:
            return 55;
        case BSONType::code:
            return 60;
        case BSONType::codeWScope:
            return 65;
        default:
            // As all possible values are mapped in the BSONType enum, we'll return a signal value
            // to be checked (if desired) by callers of the method if something completely
            // unexpected comes in. This codepath would only be reached if the given BSONType is not
            // a valid BSONType. One way this could occur is if the given value was constructed by
            // forcibly casting something into a BSONType without checking if it's valid.
            const auto ret = std::numeric_limits<std::int8_t>::min();
            // To explicitly make it different that all BSONTypes
            static_assert(ret < stdx::to_underlying(BSONType::minKey));
            return ret;
    }
}

/** Returns a number for where a given type falls in the sort order.
 *  Elements with the same return value should be compared for value equality.
 *  The return value is not a BSONType and should not be treated as one.
 *  Note: if the order changes, indexes have to be re-built or there can be corruption
 */
inline int canonicalizeBSONType(BSONType type) {
    auto ret = canonicalizeBSONTypeUnsafeLookup(type);
    if (ret != std::numeric_limits<std::int8_t>::min()) {
        return ret;
    }
    msgasserted(ErrorCodes::InvalidBSONType,
                fmt::format("Invalid/undefined BSONType value was provided ({:d})", type));
}

template <BSONType value>
struct FormatKind : std::integral_constant<BSONType, value> {};

template <typename T>
struct BSONObjAppendFormat;

/**
 * Returns whether conversion to JSON should format the Date type as local timezone.
 * This is a global setting set by the systemLog.timeStampFormat server option.
 */
void setDateFormatIsLocalTimezone(bool localTimeZone);
bool dateFormatIsLocalTimezone();

namespace bsontype_detail {

/* BSONObjFallbackFormat is the trait that BSONObjAppendFormat falls back to in case there is
   no explicit specialization for a type. It has a second templated parameter so it can be enabled
   for groups of types, e.g. enums. */
template <typename T, typename = void>
struct BSONObjFallbackFormat {};

template <typename T>
struct BSONObjFallbackFormat<T, std::enable_if_t<std::is_enum_v<T>>>
    : FormatKind<BSONType::numberInt> {};

/** This is a special case because long long and int64_t are the same on some platforms but
 * different on others. If they are the same, the long long partial specialization of
 * BSONObjAppendFormat is accepted, otherwise the int64_t partial specialization of
 * BSONObjFallbackFormat is chosen. */
template <>
struct BSONObjFallbackFormat<std::int64_t> : FormatKind<BSONType::numberLong> {};

/** Determine if T is appendable based on whether or not BSONOBjAppendFormat<T> has a value. */
template <typename T, typename = void>
struct IsBSONObjAppendable : std::false_type {};

template <typename T>
struct IsBSONObjAppendable<T, std::void_t<decltype(BSONObjAppendFormat<T>::value)>>
    : std::true_type {};

}  // namespace bsontype_detail

template <typename T>
using IsBSONObjAppendable = bsontype_detail::IsBSONObjAppendable<T>;

template <typename T>
struct BSONObjAppendFormat : bsontype_detail::BSONObjFallbackFormat<T> {};

template <>
struct BSONObjAppendFormat<bool> : FormatKind<BSONType::boolean> {};

template <>
struct BSONObjAppendFormat<char> : FormatKind<BSONType::numberInt> {};

template <>
struct BSONObjAppendFormat<unsigned char> : FormatKind<BSONType::numberInt> {};

template <>
struct BSONObjAppendFormat<short> : FormatKind<BSONType::numberInt> {};

template <>
struct BSONObjAppendFormat<unsigned short> : FormatKind<BSONType::numberInt> {};

template <>
struct BSONObjAppendFormat<int> : FormatKind<BSONType::numberInt> {};

/* For platforms where long long and int64_t are the same, this partial specialization will be
   used for both. Otherwise, int64_t will use the specialization above. */
template <>
struct BSONObjAppendFormat<long long> : FormatKind<BSONType::numberLong> {};

template <>
struct BSONObjAppendFormat<Counter64> : FormatKind<BSONType::numberLong> {};

template <>
struct BSONObjAppendFormat<Decimal128> : FormatKind<BSONType::numberDecimal> {};

template <>
struct BSONObjAppendFormat<double> : FormatKind<BSONType::numberDouble> {};

template <>
struct BSONObjAppendFormat<float> : FormatKind<BSONType::numberDouble> {};

}  // namespace mongo

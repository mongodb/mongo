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

#include <boost/optional.hpp>
#include <cstdint>
#include <iosfwd>
#include <type_traits>

#include "mongo/base/counter.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class BSONArrayBuilder;
class BSONElement;
class BSONElementCmpWithoutField;
class BSONObj;
class BSONObjBuilder;
class BSONObjBuilderValueStream;
class BSONObjIterator;
class Ordering;
struct BSONArray;  // empty subclass of BSONObj useful for overloading

extern const BSONObj kMaxBSONKey;
extern const BSONObj kMinBSONKey;

/**
    determines BSON types considered valid by validate
*/
enum class BSONVersion { kV1_0, kV1_1, kLatest = kV1_1 };

/**
    the complete list of valid BSON types
    see also bsonspec.org
*/
enum BSONType {
    /** smaller than all other types */
    MinKey = -1,
    /** end of object */
    EOO = 0,
    /** double precision floating point value */
    NumberDouble = 1,
    /** character string, stored in utf8 */
    String = 2,
    /** an embedded object */
    Object = 3,
    /** an embedded array */
    Array = 4,
    /** binary data */
    BinData = 5,
    /** Undefined type */
    Undefined = 6,
    /** ObjectId */
    jstOID = 7,
    /** boolean type */
    Bool = 8,
    /** date type */
    Date = 9,
    /** null type */
    jstNULL = 10,
    /** regular expression, a pattern with options */
    RegEx = 11,
    /** deprecated / will be redesigned */
    DBRef = 12,
    /** deprecated / use CodeWScope */
    Code = 13,
    /** a programming language (e.g., Python) symbol */
    Symbol = 14,
    /** javascript code that can execute on the database server, with SavedContext */
    CodeWScope = 15,
    /** 32 bit signed integer */
    NumberInt = 16,
    /** Two 32 bit signed integers */
    bsonTimestamp = 17,
    /** 64 bit integer */
    NumberLong = 18,
    /** 128 bit decimal */
    NumberDecimal = 19,
    /** max type that is not MaxKey */
    JSTypeMax = 19,
    /** larger than all other types */
    MaxKey = 127
};

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

inline bool isNumericBSONType(BSONType type) {
    switch (type) {
        case NumberDouble:
        case NumberInt:
        case NumberLong:
        case NumberDecimal:
            return true;
        default:
            return false;
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
    Encrypt = 6, /* encryption placeholder or encrypted data */
    bdtCustom = 128
};

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
 *  Note: if the order changes, indexes have to be re-built or than can be corruption
 */
inline int canonicalizeBSONType(BSONType type) {
    switch (type) {
        case MinKey:
        case MaxKey:
            return type;
        case EOO:
        case Undefined:
            return 0;
        case jstNULL:
            return 5;
        case NumberDecimal:
        case NumberDouble:
        case NumberInt:
        case NumberLong:
            return 10;
        case mongo::String:
        case Symbol:
            return 15;
        case Object:
            return 20;
        case mongo::Array:
            return 25;
        case BinData:
            return 30;
        case jstOID:
            return 35;
        case mongo::Bool:
            return 40;
        case mongo::Date:
            return 45;
        case bsonTimestamp:
            return 47;
        case RegEx:
            return 50;
        case DBRef:
            return 55;
        case Code:
            return 60;
        case CodeWScope:
            return 65;
        default:
            verify(0);
            return -1;
    }
}

template <BSONType value>
struct FormatKind : std::integral_constant<BSONType, value> {};

template <typename T>
struct BSONObjAppendFormat;

namespace bsontype_detail {

/* BSONObjFallbackFormat is the trait that BSONObjAppendFormat falls back to in case there is
   no explicit specialization for a type. It has a second templated parameter so it can be enabled
   for groups of types, e.g. enums. */
template <typename T, typename = void>
struct BSONObjFallbackFormat {};

template <typename T>
struct BSONObjFallbackFormat<T, std::enable_if_t<std::is_enum_v<T>>> : FormatKind<NumberInt> {};

/** This is a special case because long long and int64_t are the same on some platforms but
 * different on others. If they are the same, the long long partial specialization of
 * BSONObjAppendFormat is accepted, otherwise the int64_t partial specialization of
 * BSONObjFallbackFormat is chosen. */
template <>
struct BSONObjFallbackFormat<std::int64_t> : FormatKind<NumberLong> {};

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
struct BSONObjAppendFormat<bool> : FormatKind<Bool> {};

template <>
struct BSONObjAppendFormat<char> : FormatKind<NumberInt> {};

template <>
struct BSONObjAppendFormat<unsigned char> : FormatKind<NumberInt> {};

template <>
struct BSONObjAppendFormat<short> : FormatKind<NumberInt> {};

template <>
struct BSONObjAppendFormat<unsigned short> : FormatKind<NumberInt> {};

template <>
struct BSONObjAppendFormat<int> : FormatKind<NumberInt> {};

/* For platforms where long long and int64_t are the same, this partial specialization will be
   used for both. Otherwise, int64_t will use the specialization above. */
template <>
struct BSONObjAppendFormat<long long> : FormatKind<NumberLong> {};

template <>
struct BSONObjAppendFormat<Counter64> : FormatKind<NumberLong> {};

template <>
struct BSONObjAppendFormat<Decimal128> : FormatKind<NumberDecimal> {};

template <>
struct BSONObjAppendFormat<double> : FormatKind<NumberDouble> {};

template <>
struct BSONObjAppendFormat<float> : FormatKind<NumberDouble> {};

}  // namespace mongo

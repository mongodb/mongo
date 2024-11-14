/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "serialization_options.h"

#include "mongo/db/query/query_shape/serialization_options.h"

#include <absl/container/node_hash_map.h>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {

// We'll pre-declare all of these strings so that we can avoid the allocations when we reference
// them later.
static constexpr StringData kUndefinedTypeString = "?undefined"_sd;
static constexpr StringData kStringTypeString = "?string"_sd;
static constexpr StringData kNumberTypeString = "?number"_sd;
static constexpr StringData kMinKeyTypeString = "?minKey"_sd;
static constexpr StringData kObjectTypeString = "?object"_sd;
static constexpr StringData kArrayTypeString = "?array"_sd;
static constexpr StringData kBinDataTypeString = "?binData"_sd;
static constexpr StringData kObjectIdTypeString = "?objectId"_sd;
static constexpr StringData kBoolTypeString = "?bool"_sd;
static constexpr StringData kDateTypeString = "?date"_sd;
static constexpr StringData kNullTypeString = "?null"_sd;
static constexpr StringData kRegexTypeString = "?regex"_sd;
static constexpr StringData kDbPointerTypeString = "?dbPointer"_sd;
static constexpr StringData kJavascriptTypeString = "?javascript"_sd;
static constexpr StringData kJavascriptWithScopeTypeString = "?javascriptWithScope"_sd;
static constexpr StringData kTimestampTypeString = "?timestamp"_sd;
static constexpr StringData kMaxKeyTypeString = "?maxKey"_sd;

static const StringMap<StringData> kArrayTypeStringConstants{
    {kUndefinedTypeString.rawData(), "?array<?undefined>"_sd},
    {kStringTypeString.rawData(), "?array<?string>"_sd},
    {kNumberTypeString.rawData(), "?array<?number>"_sd},
    {kMinKeyTypeString.rawData(), "?array<?minKey>"_sd},
    {kObjectTypeString.rawData(), "?array<?object>"_sd},
    {kArrayTypeString.rawData(), "?array<?array>"_sd},
    {kBinDataTypeString.rawData(), "?array<?binData>"_sd},
    {kObjectIdTypeString.rawData(), "?array<?objectId>"_sd},
    {kBoolTypeString.rawData(), "?array<?bool>"_sd},
    {kDateTypeString.rawData(), "?array<?date>"_sd},
    {kNullTypeString.rawData(), "?array<?null>"_sd},
    {kRegexTypeString.rawData(), "?array<?regex>"_sd},
    {kDbPointerTypeString.rawData(), "?array<?dbPointer>"_sd},
    {kJavascriptTypeString.rawData(), "?array<?javascript>"_sd},
    {kJavascriptWithScopeTypeString.rawData(), "?array<?javascriptWithScope>"_sd},
    {kTimestampTypeString.rawData(), "?array<?timestamp>"_sd},
    {kMaxKeyTypeString.rawData(), "?array<?maxKey>"_sd},
};

static constexpr auto kRepresentativeString = "?"_sd;
static constexpr auto kRepresentativeNumber = 1;
static const auto kRepresentativeObject = BSON("?"
                                               << "?");
static const auto kRepresentativeArray = BSONArray();
static constexpr auto kRepresentativeBinData = BSONBinData();
static const auto kRepresentativeObjectId = OID::max();
static constexpr auto kRepresentativeBool = true;
static const auto kRepresentativeDate = Date_t::fromMillisSinceEpoch(0);
static const auto kRepresentativeRegex = BSONRegEx("/\?/");
static const auto kRepresentativeDbPointer = BSONDBRef("?.?", OID::max());
static const auto kRepresentativeJavascript = BSONCode("return ?;");
static const auto kRepresentativeJavascriptWithScope = BSONCodeWScope("return ?;", BSONObj());
static const auto kRepresentativeTimestamp = Timestamp::min();

/**
 * A default redaction strategy that generates easy to check results for testing purposes.
 */
std::string applyHmacForTest(StringData s) {
    // Avoid ending in a parenthesis since the results will occur in a raw string where the )"
    // sequence will accidentally terminate the string.
    return str::stream() << "HASH<" << s << ">";
}

/**
 * Computes a debug string meant to represent "any value of type t", where "t" is the type of the
 * provided argument. For example "?number" for any number (int, double, etc.).
 */
StringData debugTypeString(BSONType t) {
    // This is tightly coupled with 'canonicalizeBSONType' and therefore also with
    // sorting/comparison semantics.
    switch (t) {
        case EOO:
        case Undefined:
            return kUndefinedTypeString;
        case Symbol:
        case String:
            return kStringTypeString;
        case NumberInt:
        case NumberLong:
        case NumberDouble:
        case NumberDecimal:
            return kNumberTypeString;
        case MinKey:
            return kMinKeyTypeString;
        case Object:
            return kObjectTypeString;
        case Array:
            // This case should only happen if we have an array within an array.
            return kArrayTypeString;
        case BinData:
            return kBinDataTypeString;
        case jstOID:
            return kObjectIdTypeString;
        case Bool:
            return kBoolTypeString;
        case Date:
            return kDateTypeString;
        case jstNULL:
            return kNullTypeString;
        case RegEx:
            return kRegexTypeString;
        case DBRef:
            return kDbPointerTypeString;
        case Code:
            return kJavascriptTypeString;
        case CodeWScope:
            return kJavascriptWithScopeTypeString;
        case bsonTimestamp:
            return kTimestampTypeString;
        case MaxKey:
            return kMaxKeyTypeString;
        default:
            MONGO_UNREACHABLE_TASSERT(7539806);
    }
}

/**
 * Returns an arbitrary value of the same type as the one given. For any number, this will be the
 * number 1. For any boolean this will be true.
 * TODO if you need a different value to make sure it will parse, you should not use this API.
 */
ImplicitValue defaultLiteralOfType(BSONType t) {
    // This is tightly coupled with 'canonicalizeBSONType' and therefore also with
    // sorting/comparison semantics.
    switch (t) {
        case EOO:
        case Undefined:
            return BSONUndefined;
        case Symbol:
        case String:
            return kRepresentativeString;
        case NumberInt:
        case NumberLong:
        case NumberDouble:
        case NumberDecimal:
            return kRepresentativeNumber;
        case MinKey:
            return MINKEY;
        case Object:
            return kRepresentativeObject;
        case Array:
            // This case should only happen if we have an array within an array.
            return kRepresentativeArray;
        case BinData:
            return kRepresentativeBinData;
        case jstOID:
            return kRepresentativeObjectId;
        case Bool:
            return kRepresentativeBool;
        case Date:
            return kRepresentativeDate;
        case jstNULL:
            return BSONNULL;
        case RegEx:
            return kRepresentativeRegex;
        case DBRef:
            return kRepresentativeDbPointer;
        case Code:
            return kRepresentativeJavascript;
        case CodeWScope:
            return kRepresentativeJavascriptWithScope;
        case bsonTimestamp:
            return kRepresentativeTimestamp;
        case MaxKey:
            return MAXKEY;
        default:
            MONGO_UNREACHABLE_TASSERT(7539803);
    }
}

/**
 * A struct representing the sub-type information for an array.
 */
struct ArraySubtypeInfo {
    /**
     * Whether the values of an array are all the same BSON type or not (mixed).
     */
    enum class NTypes { kEmpty, kOneType, kMixed };
    ArraySubtypeInfo(NTypes nTypes_) : nTypes(nTypes_) {}
    ArraySubtypeInfo(BSONType oneType) : nTypes(NTypes::kOneType), singleType(oneType) {}

    NTypes nTypes;
    boost::optional<BSONType> singleType = boost::none;
};

template <typename ValueType>
using GetTypeFn = std::function<BSONType(ValueType)>;

static GetTypeFn<BSONElement> getBSONElementType = [](const BSONElement& e) {
    return e.type();
};
static GetTypeFn<Value> getValueType = [](const Value& v) {
    return v.getType();
};

/**
 * Scans 'arrayOfValues' to see if all values are of the same type or not. Returns this info in a
 * struct - see the struct definition for how it is represented.
 *
 * Templated algorithm to handle both iterators of BSONElements or iterators of Values.
 * 'getTypeCallback' is provided to abstract away the different '.type()' vs '.getType()' APIs.
 */
template <typename ArrayType, typename ValueType>
ArraySubtypeInfo determineArraySubType(const ArrayType& arrayOfValues,
                                       GetTypeFn<ValueType> getTypeCallback) {
    boost::optional<BSONType> firstType = boost::none;
    for (auto&& v : arrayOfValues) {
        if (!firstType) {
            firstType.emplace(getTypeCallback(v));
        } else if (*firstType != getTypeCallback(v)) {
            return {ArraySubtypeInfo::NTypes::kMixed};
        }
    }
    return firstType ? ArraySubtypeInfo{*firstType}
                     : ArraySubtypeInfo{ArraySubtypeInfo::NTypes::kEmpty};
}

ArraySubtypeInfo determineArraySubType(const BSONObj& arrayAsObj) {
    return determineArraySubType<BSONObj, BSONElement>(arrayAsObj, getBSONElementType);
}
ArraySubtypeInfo determineArraySubType(const std::vector<Value>& values) {
    return determineArraySubType<std::vector<Value>, Value>(values, getValueType);
}

template <typename ValueType>
StringData debugTypeString(
    const ValueType& v,
    GetTypeFn<ValueType> getTypeCallback,
    std::function<ArraySubtypeInfo(ValueType)> determineArraySubTypeCallback) {
    if (getTypeCallback(v) == BSONType::Array) {
        // Iterating the array as .Obj(), as if it were a BSONObj (with field names '0', '1', etc.)
        // is faster than converting the whole thing to an array which would force a copy.
        auto typeInfo = determineArraySubTypeCallback(v);
        switch (typeInfo.nTypes) {
            case ArraySubtypeInfo::NTypes::kEmpty:
                return "[]"_sd;
            case ArraySubtypeInfo::NTypes::kOneType:
                return kArrayTypeStringConstants.at(debugTypeString(*typeInfo.singleType));
            case ArraySubtypeInfo::NTypes::kMixed:
                return "?array<>";
            default:
                MONGO_UNREACHABLE_TASSERT(7539801);
        }
    }
    return debugTypeString(getTypeCallback(v));
}

template <typename ValueType>
ImplicitValue defaultLiteralOfType(
    const ValueType& v,
    GetTypeFn<ValueType> getTypeCallback,
    std::function<ArraySubtypeInfo(ValueType)> determineArraySubTypeCallback) {
    if (getTypeCallback(v) == BSONType::Array) {
        auto typeInfo = determineArraySubTypeCallback(v);
        switch (typeInfo.nTypes) {
            case ArraySubtypeInfo::NTypes::kEmpty:
                return BSONArray();
            case ArraySubtypeInfo::NTypes::kOneType:
                return std::vector<Value>{defaultLiteralOfType(*typeInfo.singleType)};
            case ArraySubtypeInfo::NTypes::kMixed:
                // We don't care which types, we'll use a number and a string as the canonical
                // mixed type array regardless. This is to ensure we don't get 2^N possibilities
                // for mixed type scenarios - we wish to collapse all "mixed type" arrays to one
                // canonical mix. The choice of int and string is mostly arbitrary - hopefully
                // somewhat comprehensible at a glance.
                return std::vector<Value>{Value(2), Value("or more types"_sd)};
            default:
                MONGO_UNREACHABLE_TASSERT(7539805);
        }
    }
    return defaultLiteralOfType(getTypeCallback(v));
}

ArraySubtypeInfo getSubTypeFromBSONElemArray(BSONElement arrayElem) {
    // Iterating the array as .Obj(), as if it were a BSONObj (with field names '0', '1', etc.)
    // is faster than converting the whole thing to an array which would force a copy.
    return determineArraySubType(arrayElem.Obj());
}
ArraySubtypeInfo getSubTypeFromValueArray(const Value& arrayVal) {
    return determineArraySubType(arrayVal.getArray());
}

void appendDefaultOfNonArrayType(BSONObjBuilder* bob, StringData name, const BSONElement& e) {
    switch (e.type()) {
        case EOO:
        case Undefined:
            bob->appendUndefined(name);
            return;
        case Symbol:
        case String:
            bob->append(name, kRepresentativeString);
            return;
        case NumberInt:
        case NumberLong:
        case NumberDouble:
        case NumberDecimal:
            bob->append(name, kRepresentativeNumber);
            return;
        case MinKey:
            bob->appendMinKey(name);
            return;
        case Object:
            bob->append(name, kRepresentativeObject);
            return;
        case Array:
            // This case is more complicated and callers should use a more generic helper.
            MONGO_UNREACHABLE_TASSERT(8094100);
        case BinData:
            bob->append(name, kRepresentativeBinData);
            return;
        case jstOID:
            bob->append(name, kRepresentativeObjectId);
            return;
        case Bool:
            bob->append(name, kRepresentativeBool);
            return;
        case Date:
            bob->append(name, kRepresentativeDate);
            return;
        case jstNULL:
            bob->appendNull(name);
            return;
        case RegEx:
            bob->append(name, kRepresentativeRegex);
            return;
        case DBRef:
            bob->append(name, kRepresentativeDbPointer);
            return;
        case Code:
            bob->append(name, kRepresentativeJavascript);
            return;
        case CodeWScope:
            bob->append(name, kRepresentativeJavascriptWithScope);
            return;
        case bsonTimestamp:
            bob->append(name, kRepresentativeTimestamp);
            return;
        case MaxKey:
            bob->appendMaxKey(name);
            return;
        default:
            MONGO_UNREACHABLE_TASSERT(8094101);
    };
}
}  // namespace

const SerializationOptions SerializationOptions::kRepresentativeQueryShapeSerializeOptions =
    SerializationOptions{.literalPolicy =
                             LiteralSerializationPolicy::kToRepresentativeParseableValue,
                         .inMatchExprSortAndDedupElements = false};

const SerializationOptions SerializationOptions::kDebugQueryShapeSerializeOptions =
    SerializationOptions{.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString,
                         .inMatchExprSortAndDedupElements = false};

const SerializationOptions SerializationOptions::kMarkIdentifiers_FOR_TEST = SerializationOptions{
    .transformIdentifiers = true, .transformIdentifiersCallback = applyHmacForTest};

const SerializationOptions SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST =
    SerializationOptions{.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString,
                         .transformIdentifiers = true,
                         .transformIdentifiersCallback = applyHmacForTest};

// Overloads for BSONElem and Value.
StringData debugTypeString(BSONElement e) {
    return debugTypeString<BSONElement>(e, getBSONElementType, getSubTypeFromBSONElemArray);
}
StringData debugTypeString(const Value& v) {
    return debugTypeString<Value>(v, getValueType, getSubTypeFromValueArray);
}

// Overloads for BSONElem and Value.
ImplicitValue defaultLiteralOfType(const Value& v) {
    return defaultLiteralOfType<Value>(v, getValueType, getSubTypeFromValueArray);
}
ImplicitValue defaultLiteralOfType(BSONElement e) {
    return defaultLiteralOfType<BSONElement>(e, getBSONElementType, getSubTypeFromBSONElemArray);
}

void SerializationOptions::appendLiteral(BSONObjBuilder* bob, const BSONElement& e) const {
    appendLiteral(bob, e.fieldNameStringData(), e);
}
void SerializationOptions::appendLiteral(BSONObjBuilder* bob,
                                         StringData name,
                                         const BSONElement& e) const {
    // The first two cases are particularly performance sensitive. We could answer everything here
    // with the code inside the 'kToDebugTypeString' branch, but there are some relatively easy ways
    // to accomplish the first two policy cases (in the common cases), so we'll special case those
    // in order to avoid constructing a temporary Value.
    switch (literalPolicy) {
        case LiteralSerializationPolicy::kUnchanged:
            bob->appendAs(e, name);
            return;
        case LiteralSerializationPolicy::kToRepresentativeParseableValue: {
            if (e.type() != BSONType::Array) {
                appendDefaultOfNonArrayType(bob, name, e);
                return;
            }
            // If it's an array we'll default to the slow but general codepath below.
            [[fallthrough]];
        }
        case LiteralSerializationPolicy::kToDebugTypeString: {
            // Performance isn't as sensitive here.
            return serializeLiteral(e).addToBsonObj(bob, name);
        }
        default:
            MONGO_UNREACHABLE_TASSERT(8094102);
    }
}

void SerializationOptions::appendLiteral(BSONObjBuilder* bob,
                                         StringData fieldName,
                                         const ImplicitValue& v,
                                         const boost::optional<Value>& representativeValue) const {
    serializeLiteral(v, representativeValue).addToBsonObj(bob, fieldName);
}

Value SerializationOptions::serializeLiteral(
    const BSONElement& e, const boost::optional<Value>& representativeValue) const {
    switch (literalPolicy) {
        case LiteralSerializationPolicy::kUnchanged:
            return Value(e);
        case LiteralSerializationPolicy::kToDebugTypeString:
            return Value(debugTypeString(e));
        case LiteralSerializationPolicy::kToRepresentativeParseableValue:
            return representativeValue.value_or(defaultLiteralOfType(e));
        default:
            MONGO_UNREACHABLE_TASSERT(7539802);
    }
}

Value SerializationOptions::serializeLiteral(
    const ImplicitValue& v, const boost::optional<Value>& representativeValue) const {
    switch (literalPolicy) {
        case LiteralSerializationPolicy::kUnchanged:
            return v;
        case LiteralSerializationPolicy::kToDebugTypeString:
            return Value(debugTypeString(v));
        case LiteralSerializationPolicy::kToRepresentativeParseableValue:
            return representativeValue.value_or(defaultLiteralOfType(v));
        default:
            MONGO_UNREACHABLE_TASSERT(7539804);
    }
}

std::string SerializationOptions::serializeFieldPathFromString(StringData path) const {
    if (transformIdentifiers) {
        try {
            return serializeFieldPath(FieldPath(path, false, false));
        } catch (DBException& ex) {
            LOGV2_DEBUG(7549808,
                        1,
                        "Failed to convert a path string to a FieldPath",
                        "pathString"_attr = path,
                        "failure"_attr = ex.toStatus());
            return serializeFieldPath("invalidFieldPathPlaceholder");
        }
    }
    return path.toString();
}
}  // namespace mongo

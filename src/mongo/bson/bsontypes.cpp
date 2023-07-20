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

#include <absl/container/flat_hash_map.h>
#include <boost/none.hpp>
#include <fmt/format.h>
#include <ostream>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/string_map.h"

namespace mongo {
namespace {
bool localTimeZoneForDate = false;
}
using namespace fmt::literals;

const char kMaxKeyData[] = {7, 0, 0, 0, static_cast<char>(MaxKey), 0, 0};
const BSONObj kMaxBSONKey(kMaxKeyData);

const char kMinKeyData[] = {7, 0, 0, 0, static_cast<char>(MinKey), 0, 0};
const BSONObj kMinBSONKey(kMinKeyData);

/* take a BSONType and return the name of that type as a char* */
const char* typeName(BSONType type) {
    switch (type) {
        case MinKey:
            return "minKey";
        case EOO:
            return "missing";
        case NumberDouble:
            return "double";
        case String:
            return "string";
        case Object:
            return "object";
        case Array:
            return "array";
        case BinData:
            return "binData";
        case Undefined:
            return "undefined";
        case jstOID:
            return "objectId";
        case Bool:
            return "bool";
        case Date:
            return "date";
        case jstNULL:
            return "null";
        case RegEx:
            return "regex";
        case DBRef:
            return "dbPointer";
        case Code:
            return "javascript";
        case Symbol:
            return "symbol";
        case CodeWScope:
            return "javascriptWithScope";
        case NumberInt:
            return "int";
        case bsonTimestamp:
            return "timestamp";
        case NumberLong:
            return "long";
        case NumberDecimal:
            return "decimal";
        // JSTypeMax doesn't make sense to turn into a string; overlaps with highest-valued type
        case MaxKey:
            return "maxKey";
        default:
            return "invalid";
    }
}

boost::optional<BSONType> findBSONTypeAlias(StringData key) {
    // intentionally leaked
    static const auto& typeAliasMap =
        *new StringMap<BSONType>{{typeName(BSONType::NumberDouble), BSONType::NumberDouble},
                                 {typeName(BSONType::String), BSONType::String},
                                 {typeName(BSONType::Object), BSONType::Object},
                                 {typeName(BSONType::Array), BSONType::Array},
                                 {typeName(BSONType::BinData), BSONType::BinData},
                                 {typeName(BSONType::Undefined), BSONType::Undefined},
                                 {typeName(BSONType::jstOID), BSONType::jstOID},
                                 {typeName(BSONType::Bool), BSONType::Bool},
                                 {typeName(BSONType::Date), BSONType::Date},
                                 {typeName(BSONType::jstNULL), BSONType::jstNULL},
                                 {typeName(BSONType::RegEx), BSONType::RegEx},
                                 {typeName(BSONType::DBRef), BSONType::DBRef},
                                 {typeName(BSONType::Code), BSONType::Code},
                                 {typeName(BSONType::Symbol), BSONType::Symbol},
                                 {typeName(BSONType::CodeWScope), BSONType::CodeWScope},
                                 {typeName(BSONType::NumberInt), BSONType::NumberInt},
                                 {typeName(BSONType::bsonTimestamp), BSONType::bsonTimestamp},
                                 {typeName(BSONType::NumberLong), BSONType::NumberLong},
                                 {typeName(BSONType::NumberDecimal), BSONType::NumberDecimal},
                                 {typeName(BSONType::MaxKey), BSONType::MaxKey},
                                 {typeName(BSONType::MinKey), BSONType::MinKey}};

    auto it = typeAliasMap.find(key);
    if (it == typeAliasMap.end())
        return boost::none;
    return it->second;
}

BSONType typeFromName(StringData name) {
    auto typeAlias = findBSONTypeAlias(name);
    uassert(ErrorCodes::BadValue, "Unknown type name: {}"_format(name), typeAlias);
    return *typeAlias;
}

Status isValidBSONTypeName(StringData typeName) {
    try {
        typeFromName(typeName);
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

std::ostream& operator<<(std::ostream& stream, BSONType type) {
    return stream << typeName(type);
}

bool isValidBSONType(int type) {
    switch (type) {
        case MinKey:
        case EOO:
        case NumberDouble:
        case String:
        case Object:
        case Array:
        case BinData:
        case Undefined:
        case jstOID:
        case Bool:
        case Date:
        case jstNULL:
        case RegEx:
        case DBRef:
        case Code:
        case Symbol:
        case CodeWScope:
        case NumberInt:
        case bsonTimestamp:
        case NumberLong:
        case NumberDecimal:
        case MaxKey:
            return true;
        default:
            return false;
    }
}

const char* typeName(BinDataType type) {
    switch (type) {
        case BinDataGeneral:
            return "general";
        case Function:
            return "function";
        case ByteArrayDeprecated:
            return "byte(deprecated)";
        case bdtUUID:
            return "UUID(deprecated)";
        case newUUID:
            return "UUID";
        case MD5Type:
            return "MD5";
        case Encrypt:
            return "encrypt";
        case Column:
            return "column";
        case Sensitive:
            return "sensitive";
        case bdtCustom:
            return "Custom";
        default:
            return "invalid";
    }
}

bool isValidBinDataType(int type) {
    switch (type) {
        case BinDataGeneral:
        case Function:
        case ByteArrayDeprecated:
        case bdtUUID:
        case newUUID:
        case MD5Type:
        case Encrypt:
        case Column:
        case bdtCustom:
        case Sensitive:
            return true;
        default:
            return false;
    }
}

void setDateFormatIsLocalTimezone(bool localTimeZone) {
    localTimeZoneForDate = localTimeZone;
}
bool dateFormatIsLocalTimezone() {
    return localTimeZoneForDate;
}

}  // namespace mongo

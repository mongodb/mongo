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

#include "mongo/bson/bsontypes.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/string_map.h"

#include <ostream>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
bool localTimeZoneForDate = false;
}

const char kMaxKeyData[] = {7, 0, 0, 0, static_cast<char>(BSONType::maxKey), 0, 0};
const BSONObj kMaxBSONKey(kMaxKeyData);

const char kMinKeyData[] = {7, 0, 0, 0, static_cast<char>(BSONType::minKey), 0, 0};
const BSONObj kMinBSONKey(kMinKeyData);

/* take a BSONType and return the name of that type as a char* */
const char* typeName(BSONType type) {
    switch (type) {
        case BSONType::minKey:
            return "minKey";
        case BSONType::eoo:
            return "missing";
        case BSONType::numberDouble:
            return "double";
        case BSONType::string:
            return "string";
        case BSONType::object:
            return "object";
        case BSONType::array:
            return "array";
        case BSONType::binData:
            return "binData";
        case BSONType::undefined:
            return "undefined";
        case BSONType::oid:
            return "objectId";
        case BSONType::boolean:
            return "bool";
        case BSONType::date:
            return "date";
        case BSONType::null:
            return "null";
        case BSONType::regEx:
            return "regex";
        case BSONType::dbRef:
            return "dbPointer";
        case BSONType::code:
            return "javascript";
        case BSONType::symbol:
            return "symbol";
        case BSONType::codeWScope:
            return "javascriptWithScope";
        case BSONType::numberInt:
            return "int";
        case BSONType::timestamp:
            return "timestamp";
        case BSONType::numberLong:
            return "long";
        case BSONType::numberDecimal:
            return "decimal";
        // JSTypeMax doesn't make sense to turn into a string; overlaps with highest-valued type
        case BSONType::maxKey:
            return "maxKey";
        default:
            return "invalid";
    }
}

boost::optional<BSONType> findBSONTypeAlias(StringData key) {
    // intentionally leaked
    static const auto& typeAliasMap =
        *new StringMap<BSONType>{{typeName(BSONType::numberDouble), BSONType::numberDouble},
                                 {typeName(BSONType::string), BSONType::string},
                                 {typeName(BSONType::object), BSONType::object},
                                 {typeName(BSONType::array), BSONType::array},
                                 {typeName(BSONType::binData), BSONType::binData},
                                 {typeName(BSONType::undefined), BSONType::undefined},
                                 {typeName(BSONType::oid), BSONType::oid},
                                 {typeName(BSONType::boolean), BSONType::boolean},
                                 {typeName(BSONType::date), BSONType::date},
                                 {typeName(BSONType::null), BSONType::null},
                                 {typeName(BSONType::regEx), BSONType::regEx},
                                 {typeName(BSONType::dbRef), BSONType::dbRef},
                                 {typeName(BSONType::code), BSONType::code},
                                 {typeName(BSONType::symbol), BSONType::symbol},
                                 {typeName(BSONType::codeWScope), BSONType::codeWScope},
                                 {typeName(BSONType::numberInt), BSONType::numberInt},
                                 {typeName(BSONType::timestamp), BSONType::timestamp},
                                 {typeName(BSONType::numberLong), BSONType::numberLong},
                                 {typeName(BSONType::numberDecimal), BSONType::numberDecimal},
                                 {typeName(BSONType::maxKey), BSONType::maxKey},
                                 {typeName(BSONType::minKey), BSONType::minKey}};

    auto it = typeAliasMap.find(key);
    if (it == typeAliasMap.end())
        return boost::none;
    return it->second;
}

BSONType typeFromName(StringData name) {
    auto typeAlias = findBSONTypeAlias(name);
    uassert(ErrorCodes::BadValue, fmt::format("Unknown type name: {}", name), typeAlias);
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
        case stdx::to_underlying(BSONType::minKey):
        case stdx::to_underlying(BSONType::eoo):
        case stdx::to_underlying(BSONType::numberDouble):
        case stdx::to_underlying(BSONType::string):
        case stdx::to_underlying(BSONType::object):
        case stdx::to_underlying(BSONType::array):
        case stdx::to_underlying(BSONType::binData):
        case stdx::to_underlying(BSONType::undefined):
        case stdx::to_underlying(BSONType::oid):
        case stdx::to_underlying(BSONType::boolean):
        case stdx::to_underlying(BSONType::date):
        case stdx::to_underlying(BSONType::null):
        case stdx::to_underlying(BSONType::regEx):
        case stdx::to_underlying(BSONType::dbRef):
        case stdx::to_underlying(BSONType::code):
        case stdx::to_underlying(BSONType::symbol):
        case stdx::to_underlying(BSONType::codeWScope):
        case stdx::to_underlying(BSONType::numberInt):
        case stdx::to_underlying(BSONType::timestamp):
        case stdx::to_underlying(BSONType::numberLong):
        case stdx::to_underlying(BSONType::numberDecimal):
        case stdx::to_underlying(BSONType::maxKey):
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
        case Vector:
            return "vector";
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
        case Vector:
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

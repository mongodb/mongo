// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsontypes.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/string_map.h"

#include <ostream>
#include <string_view>
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

boost::optional<BSONType> findBSONTypeAlias(std::string_view key) {
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

BSONType typeFromName(std::string_view name) {
    auto typeAlias = findBSONTypeAlias(name);
    uassert(ErrorCodes::BadValue, fmt::format("Unknown type name: {}", name), typeAlias);
    return *typeAlias;
}

Status isValidBSONTypeName(std::string_view typeName) {
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

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

#include "mongo/db/matcher/matcher_type_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/compiler/parsers/matcher/schema/json_schema_parser.h"
#include "mongo/util/str.h"

#include <string>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

Status addAliasToTypeSet(StringData typeAlias,
                         const findBSONTypeAliasFun& aliasMapFind,
                         MatcherTypeSet* typeSet) {
    invariant(typeSet);

    if (typeAlias == MatcherTypeSet::kMatchesAllNumbersAlias) {
        typeSet->allNumbers = true;
        return Status::OK();
    }

    auto optValue = aliasMapFind(std::string{typeAlias});
    if (!optValue) {
        // The string "missing" can be returned from the $type agg expression, but is not valid for
        // use in the $type match expression predicate. Return a special error message for this
        // case.
        if (typeAlias == StringData{typeName(BSONType::eoo)}) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "'missing' is not a legal type name. To query for "
                                           "non-existence of a field, use {$exists:false}.");
        }

        return Status(ErrorCodes::BadValue,
                      str::stream() << "Unknown type name alias: " << typeAlias);
    }

    typeSet->bsonTypes.insert(*optValue);
    return Status::OK();
}

const StringMap<BSONType> MatcherTypeSet::kJsonSchemaTypeAliasMap = {
    {std::string(JSONSchemaParser::kSchemaTypeArray), BSONType::array},
    {std::string(JSONSchemaParser::kSchemaTypeBoolean), BSONType::boolean},
    {std::string(JSONSchemaParser::kSchemaTypeNull), BSONType::null},
    {std::string(JSONSchemaParser::kSchemaTypeObject), BSONType::object},
    {std::string(JSONSchemaParser::kSchemaTypeString), BSONType::string},
};

boost::optional<BSONType> MatcherTypeSet::findJsonSchemaTypeAlias(StringData key) {
    const auto& aliasMap = kJsonSchemaTypeAliasMap;
    auto it = aliasMap.find(key);
    if (it == aliasMap.end())
        return boost::none;
    return it->second;
}

StatusWith<MatcherTypeSet> MatcherTypeSet::fromStringAliases(
    std::set<StringData> typeAliases, const findBSONTypeAliasFun& aliasMapFind) {
    MatcherTypeSet typeSet;
    for (auto&& alias : typeAliases) {
        auto status = addAliasToTypeSet(alias, aliasMapFind, &typeSet);
        if (!status.isOK()) {
            return status;
        }
    }
    return typeSet;
}

void MatcherTypeSet::toBSONArray(BSONArrayBuilder* builder) const {
    if (allNumbers) {
        builder->append(MatcherTypeSet::kMatchesAllNumbersAlias);
    }

    for (auto type : bsonTypes) {
        builder->append(type);
    }
}

uint32_t MatcherTypeSet::getBSONTypeMask() const {
    uint32_t mask = 0;
    if (allNumbers) {
        mask |= (mongo::getBSONTypeMask(BSONType::numberInt) |
                 mongo::getBSONTypeMask(BSONType::numberLong) |
                 mongo::getBSONTypeMask(BSONType::numberDouble) |
                 mongo::getBSONTypeMask(BSONType::numberDecimal));
    }

    for (auto t : bsonTypes) {
        mask |= mongo::getBSONTypeMask(t);
    }

    return mask;
}

}  // namespace mongo

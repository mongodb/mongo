// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/matcher_type_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/compiler/parsers/matcher/schema/json_schema_parser.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

Status addAliasToTypeSet(std::string_view typeAlias,
                         const findBSONTypeAliasFun& aliasMapFind,
                         MatcherTypeSet* typeSet) {
    tassert(11052422, "typeSet must not be null", typeSet);

    if (typeAlias == MatcherTypeSet::kMatchesAllNumbersAlias) {
        typeSet->allNumbers = true;
        return Status::OK();
    }

    auto optValue = aliasMapFind(std::string{typeAlias});
    if (!optValue) {
        // The string "missing" can be returned from the $type agg expression, but is not valid for
        // use in the $type match expression predicate. Return a special error message for this
        // case.
        if (typeAlias == std::string_view{typeName(BSONType::eoo)}) {
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

boost::optional<BSONType> MatcherTypeSet::findJsonSchemaTypeAlias(std::string_view key) {
    const auto& aliasMap = kJsonSchemaTypeAliasMap;
    auto it = aliasMap.find(key);
    if (it == aliasMap.end())
        return boost::none;
    return it->second;
}

StatusWith<MatcherTypeSet> MatcherTypeSet::fromStringAliases(
    std::set<std::string_view> typeAliases, const findBSONTypeAliasFun& aliasMapFind) {
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

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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/matcher_type_set.h"

#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/schema/json_schema_parser.h"

namespace mongo {
namespace {

/**
 * Adds the type represented by 'typeAlias' to 'typeSet', using 'aliasMap' as the mapping from
 * string to BSON type.
 *
 * Returns a non-OK status if 'typeAlias' does not represent a valid type.
 */
Status addAliasToTypeSet(StringData typeAlias,
                         const findBSONTypeAliasFun& aliasMapFind,
                         MatcherTypeSet* typeSet) {
    invariant(typeSet);

    if (typeAlias == MatcherTypeSet::kMatchesAllNumbersAlias) {
        typeSet->allNumbers = true;
        return Status::OK();
    }

    auto optValue = aliasMapFind(typeAlias.toString());
    if (!optValue) {
        // The string "missing" can be returned from the $type agg expression, but is not valid for
        // use in the $type match expression predicate. Return a special error message for this
        // case.
        if (typeAlias == StringData{typeName(BSONType::EOO)}) {
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

/**
 * Parses an element containing either a numerical type code or a string type alias and adds the
 * resulting type to 'typeSet'. The 'aliasMapFind' function is used to map strings to BSON types.
 *
 * Returns a non-OK status if 'elt' does not represent a valid type.
 */
Status parseSingleType(BSONElement elt,
                       const findBSONTypeAliasFun& aliasMapFind,
                       MatcherTypeSet* typeSet) {
    if (!elt.isNumber() && elt.type() != BSONType::String) {
        return Status(ErrorCodes::TypeMismatch, "type must be represented as a number or a string");
    }

    if (elt.type() == BSONType::String) {
        return addAliasToTypeSet(elt.valueStringData(), aliasMapFind, typeSet);
    }

    auto valueAsInt = elt.parseIntegerElementToInt();
    if (!valueAsInt.isOK()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid numerical type code: " << elt.number());
    }

    if (valueAsInt.getValue() == 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid numerical type code: " << elt.number()
                                    << ". Instead use {$exists:false}.");
    }

    if (!isValidBSONType(valueAsInt.getValue())) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid numerical type code: " << elt.number());
    }

    typeSet->bsonTypes.insert(static_cast<BSONType>(valueAsInt.getValue()));
    return Status::OK();
}

}  // namespace

constexpr StringData MatcherTypeSet::kMatchesAllNumbersAlias;

const StringMap<BSONType> MatcherTypeSet::kJsonSchemaTypeAliasMap = {
    {std::string(JSONSchemaParser::kSchemaTypeArray), BSONType::Array},
    {std::string(JSONSchemaParser::kSchemaTypeBoolean), BSONType::Bool},
    {std::string(JSONSchemaParser::kSchemaTypeNull), BSONType::jstNULL},
    {std::string(JSONSchemaParser::kSchemaTypeObject), BSONType::Object},
    {std::string(JSONSchemaParser::kSchemaTypeString), BSONType::String},
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

StatusWith<MatcherTypeSet> MatcherTypeSet::parse(BSONElement elt) {
    MatcherTypeSet typeSet;

    if (elt.type() != BSONType::Array) {
        auto status = parseSingleType(elt, findBSONTypeAlias, &typeSet);
        if (!status.isOK()) {
            return status;
        }
        return typeSet;
    }

    for (auto&& typeArrayElt : elt.embeddedObject()) {
        auto status = parseSingleType(typeArrayElt, findBSONTypeAlias, &typeSet);
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

BSONTypeSet BSONTypeSet::parseFromBSON(const BSONElement& element) {
    // BSON type can be specified with a type alias, other values will be rejected.
    auto typeSet = uassertStatusOK(JSONSchemaParser::parseTypeSet(element, findBSONTypeAlias));
    return BSONTypeSet(typeSet);
}

void BSONTypeSet::serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
    builder->appendArray(fieldName, _typeSet.toBSONArray());
}
}  // namespace mongo

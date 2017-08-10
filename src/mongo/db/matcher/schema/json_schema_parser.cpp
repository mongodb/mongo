/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/schema/json_schema_parser.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/matcher_type_alias.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/string_map.h"

namespace mongo {

namespace {
// JSON Schema keyword constants.
constexpr StringData kSchemaAllOfKeyword = "allOf"_sd;
constexpr StringData kSchemaAnyOfKeyword = "anyOf"_sd;
constexpr StringData kSchemaExclusiveMaximumKeyword = "exclusiveMaximum"_sd;
constexpr StringData kSchemaExclusiveMinimumKeyword = "exclusiveMinimum"_sd;
constexpr StringData kSchemaMaximumKeyword = "maximum"_sd;
constexpr StringData kSchemaMaxLengthKeyword = "maxLength"_sd;
constexpr StringData kSchemaMinimumKeyword = "minimum"_sd;
constexpr StringData kSchemaMinLengthKeyword = "minLength"_sd;
constexpr StringData kSchemaMultipleOfKeyword = "multipleOf"_sd;
constexpr StringData kSchemaNotKeyword = "not"_sd;
constexpr StringData kSchemaOneOfKeyword = "oneOf"_sd;
constexpr StringData kSchemaPatternKeyword = "pattern"_sd;
constexpr StringData kSchemaPropertiesKeyword = "properties"_sd;
constexpr StringData kSchemaTypeKeyword = "type"_sd;

/**
 * Parses 'schema' to the semantically equivalent match expression. If the schema has an
 * associated path, e.g. if we are parsing the nested schema for property "myProp" in
 *
 * {properties: {myProp: <nested-schema>}}
 *
 * then this is passed in 'path'. In this example, the value of 'path' is "myProp". If there is
 * no path, e.g. for top-level schemas, then 'path' is empty.
 */
StatusWithMatchExpression _parse(StringData path, BSONObj schema);

/**
 * Constructs and returns a match expression to evaluate a JSON Schema restriction keyword.
 *
 * This handles semantic differences between the MongoDB query language and JSON Schema. MongoDB
 * match expressions which apply to a particular type will reject non-matching types, whereas JSON
 * Schema restriction keywords allow non-matching types. As an example, consider the maxItems
 * keyword. This keyword only applies in JSON Schema if the type is an array, whereas the
 * $_internalSchemaMaxItems match expression node rejects non-arrays.
 *
 * The 'restrictionType' expresses the type to which the JSON Schema restriction applies (e.g.
 * arrays for maxItems). The 'restrictionExpr' is the match expression node which can be used to
 * enforce this restriction, should the types match (e.g. $_internalSchemaMaxItems). 'statedType' is
 * a parsed representation of the JSON Schema type keyword which is in effect.
 */
std::unique_ptr<MatchExpression> makeRestriction(MatcherTypeAlias restrictionType,
                                                 std::unique_ptr<MatchExpression> restrictionExpr,
                                                 InternalSchemaTypeExpression* statedType) {
    if (statedType) {
        const bool bothNumeric = restrictionType.allNumbers &&
            (statedType->matchesAllNumbers() || isNumericBSONType(statedType->getBSONType()));
        const bool bsonTypesMatch = restrictionType.bsonType == statedType->getBSONType();

        if (bothNumeric || bsonTypesMatch) {
            // This restriction applies to the type that is already being enforced. We return the
            // restriction unmodified.
            return restrictionExpr;
        } else {
            // This restriction doesn't take any effect, since the type of the schema is different
            // from the type to which this retriction applies.
            return stdx::make_unique<AlwaysTrueMatchExpression>();
        }
    }

    // Generate and return the following expression tree:
    //
    //  (OR (<restrictionExpr>) (NOT (INTERNAL_SCHEMA_TYPE <restrictionType>))
    //
    // We need to do this because restriction keywords do not apply when a field is either not
    // present or of a different type.
    auto typeExpr = stdx::make_unique<InternalSchemaTypeExpression>();
    invariantOK(typeExpr->init(restrictionExpr->path(), restrictionType));

    auto notExpr = stdx::make_unique<NotMatchExpression>();
    invariantOK(notExpr->init(typeExpr.release()));

    auto orExpr = stdx::make_unique<OrMatchExpression>();
    orExpr->add(notExpr.release());
    orExpr->add(restrictionExpr.release());

    return std::move(orExpr);
}

StatusWith<std::unique_ptr<InternalSchemaTypeExpression>> parseType(StringData path,
                                                                    BSONElement typeElt) {
    if (!typeElt) {
        return {nullptr};
    }

    if (typeElt.type() != BSONType::String) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '" << kSchemaTypeKeyword
                                     << "' must be a string")};
    }

    auto parsedType = MatcherTypeAlias::parseFromStringAlias(typeElt.valueStringData());
    if (!parsedType.isOK()) {
        return parsedType.getStatus();
    }

    auto typeExpr = stdx::make_unique<InternalSchemaTypeExpression>();
    auto initStatus = typeExpr->init(path, parsedType.getValue());
    if (!initStatus.isOK()) {
        return initStatus;
    }

    return {std::move(typeExpr)};
}

StatusWithMatchExpression parseMaximum(StringData path,
                                       BSONElement maximum,
                                       InternalSchemaTypeExpression* typeExpr,
                                       bool isExclusiveMaximum) {
    if (!maximum.isNumber()) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '" << kSchemaMaximumKeyword
                                     << "' must be a number")};
    }

    if (path.empty()) {
        // This restriction has no effect in a top-level schema, since we only store objects.
        return {stdx::make_unique<AlwaysTrueMatchExpression>()};
    }

    std::unique_ptr<ComparisonMatchExpression> expr;
    if (isExclusiveMaximum) {
        expr = stdx::make_unique<LTMatchExpression>();
    } else {
        expr = stdx::make_unique<LTEMatchExpression>();
    }
    auto status = expr->init(path, maximum);
    if (!status.isOK()) {
        return status;
    }

    MatcherTypeAlias restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(restrictionType, std::move(expr), typeExpr);
}

StatusWithMatchExpression parseMinimum(StringData path,
                                       BSONElement minimum,
                                       InternalSchemaTypeExpression* typeExpr,
                                       bool isExclusiveMinimum) {
    if (!minimum.isNumber()) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '" << kSchemaMinimumKeyword
                                     << "' must be a number")};
    }

    if (path.empty()) {
        // This restriction has no effect in a top-level schema, since we only store objects.
        return {stdx::make_unique<AlwaysTrueMatchExpression>()};
    }

    std::unique_ptr<ComparisonMatchExpression> expr;
    if (isExclusiveMinimum) {
        expr = stdx::make_unique<GTMatchExpression>();
    } else {
        expr = stdx::make_unique<GTEMatchExpression>();
    }
    auto status = expr->init(path, minimum);
    if (!status.isOK()) {
        return status;
    }

    MatcherTypeAlias restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(restrictionType, std::move(expr), typeExpr);
}

template <class T>
StatusWithMatchExpression parseStrLength(StringData path,
                                         BSONElement strLength,
                                         InternalSchemaTypeExpression* typeExpr,
                                         StringData keyword) {
    if (!strLength.isNumber()) {
        return {
            Status(ErrorCodes::TypeMismatch,
                   str::stream() << "$jsonSchema keyword '" << keyword << "' must be a number")};
    }

    auto strLengthWithStatus =
        MatchExpressionParser::parseIntegerElementToNonNegativeLong(strLength);

    if (!strLengthWithStatus.isOK()) {
        return strLengthWithStatus.getStatus();
    }

    if (path.empty()) {
        return {stdx::make_unique<AlwaysTrueMatchExpression>()};
    }

    auto expr = stdx::make_unique<T>();
    auto status = expr->init(path, strLengthWithStatus.getValue());
    if (!status.isOK()) {
        return status;
    }
    return makeRestriction(BSONType::String, std::move(expr), typeExpr);
}

StatusWithMatchExpression parsePattern(StringData path,
                                       BSONElement pattern,
                                       InternalSchemaTypeExpression* typeExpr) {
    if (pattern.type() != BSONType::String) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '" << kSchemaPatternKeyword
                                     << "' must be a string")};
    }

    if (path.empty()) {
        return {stdx::make_unique<AlwaysTrueMatchExpression>()};
    }

    auto expr = stdx::make_unique<RegexMatchExpression>();

    // JSON Schema does not allow regex flags to be specified.
    constexpr auto emptyFlags = "";
    auto status = expr->init(path, pattern.valueStringData(), emptyFlags);
    if (!status.isOK()) {
        return status;
    }
    return makeRestriction(BSONType::String, std::move(expr), typeExpr);
}

StatusWithMatchExpression parseMultipleOf(StringData path,
                                          BSONElement multipleOf,
                                          InternalSchemaTypeExpression* typeExpr) {
    if (!multipleOf.isNumber()) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '" << kSchemaMultipleOfKeyword
                                     << "' must be a number")};
    }

    if (multipleOf.numberDecimal().isNegative() || multipleOf.numberDecimal().isZero()) {
        return {Status(ErrorCodes::FailedToParse,
                       str::stream() << "$jsonSchema keyword '" << kSchemaMultipleOfKeyword
                                     << "' must have a positive value")};
    }
    if (path.empty()) {
        return {stdx::make_unique<AlwaysTrueMatchExpression>()};
    }

    auto expr = stdx::make_unique<InternalSchemaFmodMatchExpression>();
    auto status = expr->init(path, multipleOf.numberDecimal(), Decimal128(0));
    if (!status.isOK()) {
        return status;
    }

    MatcherTypeAlias restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(restrictionType, std::move(expr), typeExpr);
}

template <class T>
StatusWithMatchExpression parseLogicalKeyword(StringData path, BSONElement logicalElement) {
    if (logicalElement.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '" << logicalElement.fieldNameStringData()
                              << "' must be an array"};
    }

    auto logicalElementObj = logicalElement.embeddedObject();
    if (logicalElementObj.isEmpty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "$jsonSchema keyword '" << logicalElement.fieldNameStringData()
                              << "' must be a non-empty array"};
    }

    std::unique_ptr<T> listOfExpr = stdx::make_unique<T>();
    for (const auto& elem : logicalElementObj) {
        if (elem.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '" << logicalElement.fieldNameStringData()
                                  << "' must be an array of objects, but found an element of type "
                                  << elem.type()};
        }

        auto nestedSchemaMatch = _parse(path, elem.embeddedObject());
        if (!nestedSchemaMatch.isOK()) {
            return nestedSchemaMatch.getStatus();
        }

        listOfExpr->add(nestedSchemaMatch.getValue().release());
    }

    return {std::move(listOfExpr)};
}

/**
 * Parses the logical keywords in 'keywordMap' to their equivalent match expressions
 * and, on success, adds the results to 'andExpr'.
 *
 * This function parses the following keywords:
 *  - allOf
 *  - anyOf
 *  - oneOf
 *  - not
 *  - enum
 */
Status parseLogicalKeywords(StringMap<BSONElement>& keywordMap,
                            StringData path,
                            AndMatchExpression* andExpr) {
    if (auto allOfElt = keywordMap[kSchemaAllOfKeyword]) {
        auto allOfExpr = parseLogicalKeyword<AndMatchExpression>(path, allOfElt);
        if (!allOfExpr.isOK()) {
            return allOfExpr.getStatus();
        }
        andExpr->add(allOfExpr.getValue().release());
    }

    if (auto anyOfElt = keywordMap[kSchemaAnyOfKeyword]) {
        auto anyOfExpr = parseLogicalKeyword<OrMatchExpression>(path, anyOfElt);
        if (!anyOfExpr.isOK()) {
            return anyOfExpr.getStatus();
        }
        andExpr->add(anyOfExpr.getValue().release());
    }

    if (auto oneOfElt = keywordMap[kSchemaOneOfKeyword]) {
        auto oneOfExpr = parseLogicalKeyword<InternalSchemaXorMatchExpression>(path, oneOfElt);
        if (!oneOfExpr.isOK()) {
            return oneOfExpr.getStatus();
        }
        andExpr->add(oneOfExpr.getValue().release());
    }

    if (auto notElt = keywordMap[kSchemaNotKeyword]) {
        if (notElt.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '" << kSchemaNotKeyword
                                  << "' must be an object, but found an element of type "
                                  << notElt.type()};
        }

        auto parsedExpr = _parse(path, notElt.embeddedObject());
        if (!parsedExpr.isOK()) {
            return parsedExpr.getStatus();
        }

        auto notMatchExpr = stdx::make_unique<NotMatchExpression>();
        auto initStatus = notMatchExpr->init(parsedExpr.getValue().release());
        if (!initStatus.isOK()) {
            return initStatus;
        }
        andExpr->add(notMatchExpr.release());
    }

    return Status::OK();
}

StatusWithMatchExpression parseProperties(StringData path,
                                          BSONElement propertiesElt,
                                          InternalSchemaTypeExpression* typeExpr) {
    if (propertiesElt.type() != BSONType::Object) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '" << kSchemaPropertiesKeyword
                                     << "' must be an object")};
    }
    auto propertiesObj = propertiesElt.embeddedObject();

    auto andExpr = stdx::make_unique<AndMatchExpression>();
    for (auto&& property : propertiesObj) {
        if (property.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Nested schema for $jsonSchema property '"
                                  << property.fieldNameStringData()
                                  << "' must be an object"};
        }

        auto nestedSchemaMatch = _parse(property.fieldNameStringData(), property.embeddedObject());
        if (!nestedSchemaMatch.isOK()) {
            return nestedSchemaMatch.getStatus();
        }

        // Each property either must not exist or must match the nested schema. Therefore, we
        // generate the match expression (OR (NOT (EXISTS)) <nestedSchemaMatch>).
        auto existsExpr = stdx::make_unique<ExistsMatchExpression>();
        invariantOK(existsExpr->init(property.fieldNameStringData()));

        auto notExpr = stdx::make_unique<NotMatchExpression>();
        invariantOK(notExpr->init(existsExpr.release()));

        auto orExpr = stdx::make_unique<OrMatchExpression>();
        orExpr->add(notExpr.release());
        orExpr->add(nestedSchemaMatch.getValue().release());

        andExpr->add(orExpr.release());
    }

    // If this is a top-level schema, then we have no path and there is no need for an
    // explicit object match node.
    if (path.empty()) {
        return {std::move(andExpr)};
    }

    auto objectMatch = stdx::make_unique<InternalSchemaObjectMatchExpression>();
    auto objectMatchStatus = objectMatch->init(std::move(andExpr), path);
    if (!objectMatchStatus.isOK()) {
        return objectMatchStatus;
    }

    return makeRestriction(BSONType::Object, std::move(objectMatch), typeExpr);
}

StatusWithMatchExpression _parse(StringData path, BSONObj schema) {
    // Map from JSON Schema keyword to the corresponding element from 'schema', or to an empty
    // BSONElement if the JSON Schema keyword is not specified.
    StringMap<BSONElement> keywordMap{
        {kSchemaAllOfKeyword, {}},
        {kSchemaAnyOfKeyword, {}},
        {kSchemaExclusiveMaximumKeyword, {}},
        {kSchemaExclusiveMinimumKeyword, {}},
        {kSchemaMaximumKeyword, {}},
        {kSchemaMaxLengthKeyword, {}},
        {kSchemaMinimumKeyword, {}},
        {kSchemaMinLengthKeyword, {}},
        {kSchemaMultipleOfKeyword, {}},
        {kSchemaNotKeyword, {}},
        {kSchemaOneOfKeyword, {}},
        {kSchemaPatternKeyword, {}},
        {kSchemaPropertiesKeyword, {}},
        {kSchemaTypeKeyword, {}},
    };

    for (auto&& elt : schema) {
        auto it = keywordMap.find(elt.fieldNameStringData());
        if (it == keywordMap.end()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Unknown $jsonSchema keyword: "
                                        << elt.fieldNameStringData());
        }

        if (it->second) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Duplicate $jsonSchema keyword: "
                                        << elt.fieldNameStringData());
        }

        keywordMap[elt.fieldNameStringData()] = elt;
    }

    auto typeExpr = parseType(path, keywordMap[kSchemaTypeKeyword]);
    if (!typeExpr.isOK()) {
        return typeExpr.getStatus();
    }

    auto andExpr = stdx::make_unique<AndMatchExpression>();

    if (auto propertiesElt = keywordMap[kSchemaPropertiesKeyword]) {
        auto propertiesExpr = parseProperties(path, propertiesElt, typeExpr.getValue().get());
        if (!propertiesExpr.isOK()) {
            return propertiesExpr;
        }
        andExpr->add(propertiesExpr.getValue().release());
    }

    if (auto maximumElt = keywordMap[kSchemaMaximumKeyword]) {
        bool isExclusiveMaximum = false;
        if (auto exclusiveMaximumElt = keywordMap[kSchemaExclusiveMaximumKeyword]) {
            if (!exclusiveMaximumElt.isBoolean()) {
                return {Status(ErrorCodes::TypeMismatch,
                               str::stream() << "$jsonSchema keyword '"
                                             << kSchemaExclusiveMaximumKeyword
                                             << "' must be a boolean")};
            } else {
                isExclusiveMaximum = exclusiveMaximumElt.boolean();
            }
        }
        auto maxExpr =
            parseMaximum(path, maximumElt, typeExpr.getValue().get(), isExclusiveMaximum);
        if (!maxExpr.isOK()) {
            return maxExpr;
        }
        andExpr->add(maxExpr.getValue().release());
    } else if (keywordMap[kSchemaExclusiveMaximumKeyword]) {
        // If "exclusiveMaximum" is present, "maximum" must also be present.
        return {Status(ErrorCodes::FailedToParse,
                       str::stream() << "$jsonSchema keyword '" << kSchemaMaximumKeyword
                                     << "' must be a present if "
                                     << kSchemaExclusiveMaximumKeyword
                                     << " is present")};
    }

    if (auto minimumElt = keywordMap[kSchemaMinimumKeyword]) {
        bool isExclusiveMinimum = false;
        if (auto exclusiveMinimumElt = keywordMap[kSchemaExclusiveMinimumKeyword]) {
            if (!exclusiveMinimumElt.isBoolean()) {
                return {Status(ErrorCodes::TypeMismatch,
                               str::stream() << "$jsonSchema keyword '"
                                             << kSchemaExclusiveMinimumKeyword
                                             << "' must be a boolean")};
            } else {
                isExclusiveMinimum = exclusiveMinimumElt.boolean();
            }
        }
        auto minExpr =
            parseMinimum(path, minimumElt, typeExpr.getValue().get(), isExclusiveMinimum);
        if (!minExpr.isOK()) {
            return minExpr;
        }
        andExpr->add(minExpr.getValue().release());
    } else if (keywordMap[kSchemaExclusiveMinimumKeyword]) {
        // If "exclusiveMinimum" is present, "minimum" must also be present.
        return {Status(ErrorCodes::FailedToParse,
                       str::stream() << "$jsonSchema keyword '" << kSchemaMinimumKeyword
                                     << "' must be a present if "
                                     << kSchemaExclusiveMinimumKeyword
                                     << " is present")};
    }

    if (auto maxLengthElt = keywordMap[kSchemaMaxLengthKeyword]) {
        auto maxLengthExpr = parseStrLength<InternalSchemaMaxLengthMatchExpression>(
            path, maxLengthElt, typeExpr.getValue().get(), kSchemaMaxLengthKeyword);
        if (!maxLengthExpr.isOK()) {
            return maxLengthExpr;
        }
        andExpr->add(maxLengthExpr.getValue().release());
    }

    if (auto minLengthElt = keywordMap[kSchemaMinLengthKeyword]) {
        auto minLengthExpr = parseStrLength<InternalSchemaMinLengthMatchExpression>(
            path, minLengthElt, typeExpr.getValue().get(), kSchemaMinLengthKeyword);
        if (!minLengthExpr.isOK()) {
            return minLengthExpr;
        }
        andExpr->add(minLengthExpr.getValue().release());
    }

    if (auto patternElt = keywordMap[kSchemaPatternKeyword]) {
        auto patternExpr = parsePattern(path, patternElt, typeExpr.getValue().get());
        if (!patternExpr.isOK()) {
            return patternExpr;
        }
        andExpr->add(patternExpr.getValue().release());
    }
    if (auto multipleOfElt = keywordMap[kSchemaMultipleOfKeyword]) {
        auto multipleOfExpr = parseMultipleOf(path, multipleOfElt, typeExpr.getValue().get());
        if (!multipleOfExpr.isOK()) {
            return multipleOfExpr;
        }
        andExpr->add(multipleOfExpr.getValue().release());
    }

    auto parseStatus = parseLogicalKeywords(keywordMap, path, andExpr.get());
    if (!parseStatus.isOK()) {
        return parseStatus;
    }

    if (path.empty() && typeExpr.getValue() &&
        typeExpr.getValue()->getBSONType() != BSONType::Object) {
        // This is a top-level schema which requires that the type is something other than
        // "object". Since we only know how to store objects, this schema matches nothing.
        return {stdx::make_unique<AlwaysFalseMatchExpression>()};
    }

    if (!path.empty() && typeExpr.getValue()) {
        andExpr->add(typeExpr.getValue().release());
    }
    return {std::move(andExpr)};
}

}  // namespace

StatusWithMatchExpression JSONSchemaParser::parse(BSONObj schema) {
    return _parse(StringData{}, schema);
}

}  // namespace mongo

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
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/string_map.h"

namespace mongo {

namespace {
// JSON Schema keyword constants.
constexpr StringData kSchemaExclusiveMaximumKeyword = "exclusiveMaximum"_sd;
constexpr StringData kSchemaExclusiveMinimumKeyword = "exclusiveMinimum"_sd;
constexpr StringData kSchemaMaximumKeyword = "maximum"_sd;
constexpr StringData kSchemaMinimumKeyword = "minimum"_sd;
constexpr StringData kSchemaPropertiesKeyword = "properties"_sd;
constexpr StringData kSchemaTypeKeyword = "type"_sd;

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
std::unique_ptr<MatchExpression> makeRestriction(TypeMatchExpression::Type restrictionType,
                                                 std::unique_ptr<MatchExpression> restrictionExpr,
                                                 TypeMatchExpression* statedType) {
    if (statedType) {
        const bool bothNumeric = restrictionType.allNumbers &&
            (statedType->matchesAllNumbers() || isNumericBSONType(statedType->getBSONType()));
        const bool bsonTypesMatch = restrictionType.bsonType == statedType->getBSONType();

        if (!bothNumeric && !bsonTypesMatch) {
            // This restriction doesn't take any effect, since the type of the schema is different
            // from the type to which this retriction applies.
            return stdx::make_unique<AlwaysTrueMatchExpression>();
        }
    }

    // Generate and return the following expression tree:
    //
    //      OR
    //    /   /
    //  NOT  <restrictionExpr>
    //  /
    // TYPE
    //  <restrictionType>
    //
    // We need to do this because restriction keywords do not apply when a field is either not
    // present or of a different type.
    auto typeExprForNot = stdx::make_unique<TypeMatchExpression>();
    invariantOK(typeExprForNot->init(restrictionExpr->path(), restrictionType));

    auto notExpr = stdx::make_unique<NotMatchExpression>(typeExprForNot.release());
    auto orExpr = stdx::make_unique<OrMatchExpression>();
    orExpr->add(notExpr.release());
    orExpr->add(restrictionExpr.release());

    return std::move(orExpr);
}

/**
 * Constructs and returns the following expression tree:
 *     OR
 *    /  \
 *  NOT   <typeExpr>
 *  /
 * EXISTS
 *  <typeExpr field>
 *
 * This is needed because the JSON Schema 'type' keyword only applies if the corresponding field is
 * present.
 *
 * 'typeExpr' must be non-null and must have a non-empty path.
 */
std::unique_ptr<MatchExpression> makeTypeRestriction(
    std::unique_ptr<TypeMatchExpression> typeExpr) {
    invariant(typeExpr);
    invariant(!typeExpr->path().empty());

    auto existsExpr = stdx::make_unique<ExistsMatchExpression>();
    invariantOK(existsExpr->init(typeExpr->path()));

    auto notExpr = stdx::make_unique<NotMatchExpression>(existsExpr.release());
    auto orExpr = stdx::make_unique<OrMatchExpression>();
    orExpr->add(notExpr.release());
    orExpr->add(typeExpr.release());

    return std::move(orExpr);
}

StatusWith<std::unique_ptr<TypeMatchExpression>> parseType(StringData path, BSONElement typeElt) {
    if (!typeElt) {
        return {nullptr};
    }

    if (typeElt.type() != BSONType::String) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '" << kSchemaTypeKeyword
                                     << "' must be a string")};
    }

    return MatchExpressionParser::parseTypeFromAlias(path, typeElt.valueStringData());
}

StatusWithMatchExpression parseMaximum(StringData path,
                                       BSONElement maximum,
                                       TypeMatchExpression* typeExpr,
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

    // We use Number as a stand-in for all numeric restrictions.
    TypeMatchExpression::Type restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(restrictionType, std::move(expr), typeExpr);
}

StatusWithMatchExpression parseMinimum(StringData path,
                                       BSONElement minimum,
                                       TypeMatchExpression* typeExpr,
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

    // We use Number as a stand-in for all numeric restrictions.
    TypeMatchExpression::Type restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(restrictionType, std::move(expr), typeExpr);
}
}  // namespace

StatusWithMatchExpression JSONSchemaParser::_parseProperties(StringData path,
                                                             BSONElement propertiesElt,
                                                             TypeMatchExpression* typeExpr) {
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
        andExpr->add(nestedSchemaMatch.getValue().release());
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

StatusWithMatchExpression JSONSchemaParser::_parse(StringData path, BSONObj schema) {
    // Map from JSON Schema keyword to the corresponding element from 'schema', or to an empty
    // BSONElement if the JSON Schema keyword is not specified.
    StringMap<BSONElement> keywordMap{{kSchemaTypeKeyword, {}},
                                      {kSchemaPropertiesKeyword, {}},
                                      {kSchemaMaximumKeyword, {}},
                                      {kSchemaMinimumKeyword, {}},
                                      {kSchemaExclusiveMaximumKeyword, {}},
                                      {kSchemaExclusiveMinimumKeyword, {}}};

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
        auto propertiesExpr = _parseProperties(path, propertiesElt, typeExpr.getValue().get());
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

    if (path.empty() && typeExpr.getValue() &&
        typeExpr.getValue()->getBSONType() != BSONType::Object) {
        // This is a top-level schema which requires that the type is something other than
        // "object". Since we only know how to store objects, this schema matches nothing.
        return {stdx::make_unique<AlwaysFalseMatchExpression>()};
    }

    if (!path.empty() && typeExpr.getValue()) {
        andExpr->add(makeTypeRestriction(std::move(typeExpr.getValue())).release());
    }
    return {std::move(andExpr)};
}

StatusWithMatchExpression JSONSchemaParser::parse(BSONObj schema) {
    return _parse(StringData{}, schema);
}

}  // namespace mongo

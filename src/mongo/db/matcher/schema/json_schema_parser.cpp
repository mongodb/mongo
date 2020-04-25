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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/schema/json_schema_parser.h"

#include <memory>

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/unordered_fields_bsonelement_comparator.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/matcher/schema/json_pointer.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/util/string_map.h"

namespace mongo {

using PatternSchema = InternalSchemaAllowedPropertiesMatchExpression::PatternSchema;
using Pattern = InternalSchemaAllowedPropertiesMatchExpression::Pattern;

namespace {

using findBSONTypeAliasFun = std::function<boost::optional<BSONType>(StringData)>;

// Explicitly unsupported JSON Schema keywords.
const std::set<StringData> unsupportedKeywords{
    "$ref"_sd,
    "$schema"_sd,
    "default"_sd,
    "definitions"_sd,
    "format"_sd,
    "id"_sd,
};

constexpr StringData kNamePlaceholder = "i"_sd;

/**
 * Parses 'schema' to the semantically equivalent match expression. If the schema has an associated
 * path, e.g. if we are parsing the nested schema for property "myProp" in
 *
 *    {properties: {myProp: <nested-schema>}}
 *
 * then this is passed in 'path'. In this example, the value of 'path' is "myProp". If there is no
 * path, e.g. for top-level schemas, then 'path' is empty.
 */
StatusWithMatchExpression _parse(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 StringData path,
                                 BSONObj schema,
                                 bool ignoreUnknownKeywords);

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
std::unique_ptr<MatchExpression> makeRestriction(const MatcherTypeSet& restrictionType,
                                                 StringData path,
                                                 std::unique_ptr<MatchExpression> restrictionExpr,
                                                 InternalSchemaTypeExpression* statedType) {
    invariant(restrictionType.isSingleType());

    if (statedType && statedType->typeSet().isSingleType()) {
        // Use NumberInt in the "number" case as a stand-in.
        BSONType statedBSONType = statedType->typeSet().allNumbers
            ? BSONType::NumberInt
            : *statedType->typeSet().bsonTypes.begin();

        if (restrictionType.hasType(statedBSONType)) {
            // This restriction applies to the type that is already being enforced. We return the
            // restriction unmodified.
            return restrictionExpr;
        } else {
            // This restriction doesn't take any effect, since the type of the schema is different
            // from the type to which this retriction applies.
            return std::make_unique<AlwaysTrueMatchExpression>();
        }
    }

    // Generate and return the following expression tree:
    //
    //  (OR (<restrictionExpr>) (NOT (INTERNAL_SCHEMA_TYPE <restrictionType>))
    //
    // We need to do this because restriction keywords do not apply when a field is either not
    // present or of a different type.
    auto typeExpr = std::make_unique<InternalSchemaTypeExpression>(path, restrictionType);

    auto notExpr = std::make_unique<NotMatchExpression>(typeExpr.release());

    auto orExpr = std::make_unique<OrMatchExpression>();
    orExpr->add(notExpr.release());
    orExpr->add(restrictionExpr.release());

    return std::move(orExpr);
}

StatusWith<std::unique_ptr<InternalSchemaTypeExpression>> parseType(
    StringData path,
    StringData keywordName,
    BSONElement typeElt,
    const findBSONTypeAliasFun& aliasMapFind) {

    auto typeSet = JSONSchemaParser::parseTypeSet(typeElt, aliasMapFind);
    if (!typeSet.isOK()) {
        return typeSet.getStatus();
    }

    if (typeSet.getValue().isEmpty()) {
        return {Status(ErrorCodes::FailedToParse,
                       str::stream() << "$jsonSchema keyword '" << keywordName
                                     << "' must name at least one type")};
    }

    auto typeExpr =
        std::make_unique<InternalSchemaTypeExpression>(path, std::move(typeSet.getValue()));

    return {std::move(typeExpr)};
}

StatusWithMatchExpression parseMaximum(StringData path,
                                       BSONElement maximum,
                                       InternalSchemaTypeExpression* typeExpr,
                                       bool isExclusiveMaximum) {
    if (!maximum.isNumber()) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaMaximumKeyword
                           << "' must be a number")};
    }

    if (path.empty()) {
        // This restriction has no effect in a top-level schema, since we only store objects.
        return {std::make_unique<AlwaysTrueMatchExpression>()};
    }

    std::unique_ptr<ComparisonMatchExpression> expr;
    if (isExclusiveMaximum) {
        expr = std::make_unique<LTMatchExpression>(path, maximum);
    } else {
        expr = std::make_unique<LTEMatchExpression>(path, maximum);
    }

    MatcherTypeSet restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(restrictionType, path, std::move(expr), typeExpr);
}

StatusWithMatchExpression parseMinimum(StringData path,
                                       BSONElement minimum,
                                       InternalSchemaTypeExpression* typeExpr,
                                       bool isExclusiveMinimum) {
    if (!minimum.isNumber()) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaMinimumKeyword
                           << "' must be a number")};
    }

    if (path.empty()) {
        // This restriction has no effect in a top-level schema, since we only store objects.
        return {std::make_unique<AlwaysTrueMatchExpression>()};
    }

    std::unique_ptr<ComparisonMatchExpression> expr;
    if (isExclusiveMinimum) {
        expr = std::make_unique<GTMatchExpression>(path, minimum);
    } else {
        expr = std::make_unique<GTEMatchExpression>(path, minimum);
    }

    MatcherTypeSet restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(restrictionType, path, std::move(expr), typeExpr);
}

/**
 * Parses length-related keywords that expect a nonnegative long as an argument.
 */
template <class T>
StatusWithMatchExpression parseLength(StringData path,
                                      BSONElement length,
                                      InternalSchemaTypeExpression* typeExpr,
                                      BSONType restrictionType) {
    auto parsedLength = length.parseIntegerElementToNonNegativeLong();
    if (!parsedLength.isOK()) {
        return parsedLength.getStatus();
    }

    if (path.empty()) {
        return {std::make_unique<AlwaysTrueMatchExpression>()};
    }

    auto expr = std::make_unique<T>(path, parsedLength.getValue());
    return makeRestriction(restrictionType, path, std::move(expr), typeExpr);
}

StatusWithMatchExpression parsePattern(StringData path,
                                       BSONElement pattern,
                                       InternalSchemaTypeExpression* typeExpr) {
    if (pattern.type() != BSONType::String) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaPatternKeyword
                           << "' must be a string")};
    }

    if (path.empty()) {
        return {std::make_unique<AlwaysTrueMatchExpression>()};
    }

    // JSON Schema does not allow regex flags to be specified.
    constexpr auto emptyFlags = "";
    auto expr = std::make_unique<RegexMatchExpression>(path, pattern.valueStringData(), emptyFlags);

    return makeRestriction(BSONType::String, path, std::move(expr), typeExpr);
}

StatusWithMatchExpression parseMultipleOf(StringData path,
                                          BSONElement multipleOf,
                                          InternalSchemaTypeExpression* typeExpr) {
    if (!multipleOf.isNumber()) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaMultipleOfKeyword
                           << "' must be a number")};
    }

    if (multipleOf.numberDecimal().isNegative() || multipleOf.numberDecimal().isZero()) {
        return {Status(ErrorCodes::FailedToParse,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaMultipleOfKeyword
                           << "' must have a positive value")};
    }
    if (path.empty()) {
        return {std::make_unique<AlwaysTrueMatchExpression>()};
    }

    auto expr = std::make_unique<InternalSchemaFmodMatchExpression>(
        path, multipleOf.numberDecimal(), Decimal128(0));

    MatcherTypeSet restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(restrictionType, path, std::move(expr), typeExpr);
}

template <class T>
StatusWithMatchExpression parseLogicalKeyword(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              StringData path,
                                              BSONElement logicalElement,
                                              bool ignoreUnknownKeywords) {
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

    std::unique_ptr<T> listOfExpr = std::make_unique<T>();
    for (const auto& elem : logicalElementObj) {
        if (elem.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '" << logicalElement.fieldNameStringData()
                                  << "' must be an array of objects, but found an element of type "
                                  << elem.type()};
        }

        auto nestedSchemaMatch = _parse(expCtx, path, elem.embeddedObject(), ignoreUnknownKeywords);
        if (!nestedSchemaMatch.isOK()) {
            return nestedSchemaMatch.getStatus();
        }

        listOfExpr->add(nestedSchemaMatch.getValue().release());
    }

    return {std::move(listOfExpr)};
}

StatusWithMatchExpression parseEnum(StringData path, BSONElement enumElement) {
    if (enumElement.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaEnumKeyword
                              << "' must be an array, but found an element of type "
                              << enumElement.type()};
    }

    auto enumArray = enumElement.embeddedObject();
    if (enumArray.isEmpty()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaEnumKeyword
                              << "' cannot be an empty array"};
    }

    auto orExpr = std::make_unique<OrMatchExpression>();
    UnorderedFieldsBSONElementComparator eltComp;
    BSONEltSet eqSet = eltComp.makeBSONEltSet();
    for (auto&& arrayElem : enumArray) {
        auto insertStatus = eqSet.insert(arrayElem);
        if (!insertStatus.second) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaEnumKeyword
                                  << "' array cannot contain duplicate values."};
        }

        // 'enum' at the top-level implies a literal object match on the root document.
        if (path.empty()) {
            // Top-level non-object enum values can be safely ignored, since MongoDB only stores
            // objects, not scalars or arrays.
            if (arrayElem.type() == BSONType::Object) {
                auto rootDocEq = std::make_unique<InternalSchemaRootDocEqMatchExpression>(
                    arrayElem.embeddedObject());
                orExpr->add(rootDocEq.release());
            }
        } else {
            auto eqExpr = std::make_unique<InternalSchemaEqMatchExpression>(path, arrayElem);

            orExpr->add(eqExpr.release());
        }
    }

    // Make sure that the OR expression has at least 1 child.
    if (orExpr->numChildren() == 0) {
        return {std::make_unique<AlwaysFalseMatchExpression>()};
    }

    return {std::move(orExpr)};
}

/**
 * Given a BSON element corresponding to the $jsonSchema "required" keyword, returns the set of
 * required property names. If the contents of the "required" keyword are invalid, returns a non-OK
 * status.
 */
StatusWith<StringDataSet> parseRequired(BSONElement requiredElt) {
    if (requiredElt.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaRequiredKeyword
                              << "' must be an array, but found an element of type "
                              << requiredElt.type()};
    }

    StringDataSet properties;
    for (auto&& propertyName : requiredElt.embeddedObject()) {
        if (propertyName.type() != BSONType::String) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '"
                                  << JSONSchemaParser::kSchemaRequiredKeyword
                                  << "' must be an array of strings, but found an element of type: "
                                  << propertyName.type()};
        }

        const auto [it, didInsert] = properties.insert(propertyName.valueStringData());
        if (!didInsert) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "$jsonSchema keyword '"
                                  << JSONSchemaParser::kSchemaRequiredKeyword
                                  << "' array cannot contain duplicate values"};
        }
    }

    if (properties.empty()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaRequiredKeyword
                              << "' cannot be an empty array"};
    }

    return std::move(properties);
}

/**
 * Given the already-parsed set of required properties, returns a MatchExpression which ensures that
 * those properties exist. Returns a parsing error if the translation fails.
 */
StatusWithMatchExpression translateRequired(const StringDataSet& requiredProperties,
                                            StringData path,
                                            InternalSchemaTypeExpression* typeExpr) {
    auto andExpr = std::make_unique<AndMatchExpression>();

    std::vector<StringData> sortedProperties(requiredProperties.begin(), requiredProperties.end());
    std::sort(sortedProperties.begin(), sortedProperties.end());
    for (auto&& propertyName : sortedProperties) {
        andExpr->add(new ExistsMatchExpression(propertyName));
    }

    // If this is a top-level schema, then we know that we are matching against objects, and there
    // is no need to worry about ensuring that non-objects match.
    if (path.empty()) {
        return {std::move(andExpr)};
    }

    auto objectMatch =
        std::make_unique<InternalSchemaObjectMatchExpression>(path, std::move(andExpr));

    return makeRestriction(BSONType::Object, path, std::move(objectMatch), typeExpr);
}

StatusWithMatchExpression parseProperties(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          StringData path,
                                          BSONElement propertiesElt,
                                          InternalSchemaTypeExpression* typeExpr,
                                          const StringDataSet& requiredProperties,
                                          bool ignoreUnknownKeywords) {
    if (propertiesElt.type() != BSONType::Object) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaPropertiesKeyword
                           << "' must be an object")};
    }
    auto propertiesObj = propertiesElt.embeddedObject();

    auto andExpr = std::make_unique<AndMatchExpression>();
    for (auto&& property : propertiesObj) {
        if (property.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Nested schema for $jsonSchema property '"
                                  << property.fieldNameStringData() << "' must be an object"};
        }

        auto nestedSchemaMatch = _parse(expCtx,
                                        property.fieldNameStringData(),
                                        property.embeddedObject(),
                                        ignoreUnknownKeywords);
        if (!nestedSchemaMatch.isOK()) {
            return nestedSchemaMatch.getStatus();
        }

        if (requiredProperties.find(property.fieldNameStringData()) != requiredProperties.end()) {
            // The field name for which we created the nested schema is a required property. This
            // property must exist and therefore must match 'nestedSchemaMatch'.
            andExpr->add(nestedSchemaMatch.getValue().release());
        } else {
            // This property either must not exist or must match the nested schema. Therefore, we
            // generate the match expression (OR (NOT (EXISTS)) <nestedSchemaMatch>).
            auto existsExpr =
                std::make_unique<ExistsMatchExpression>(property.fieldNameStringData());

            auto notExpr = std::make_unique<NotMatchExpression>(existsExpr.release());

            auto orExpr = std::make_unique<OrMatchExpression>();
            orExpr->add(notExpr.release());
            orExpr->add(nestedSchemaMatch.getValue().release());

            andExpr->add(orExpr.release());
        }
    }

    // If this is a top-level schema, then we have no path and there is no need for an
    // explicit object match node.
    if (path.empty()) {
        return {std::move(andExpr)};
    }

    auto objectMatch =
        std::make_unique<InternalSchemaObjectMatchExpression>(path, std::move(andExpr));

    return makeRestriction(BSONType::Object, path, std::move(objectMatch), typeExpr);
}

StatusWith<std::vector<PatternSchema>> parsePatternProperties(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement patternPropertiesElt,
    bool ignoreUnknownKeywords) {
    std::vector<PatternSchema> patternProperties;
    if (!patternPropertiesElt) {
        return {std::move(patternProperties)};
    }

    if (patternPropertiesElt.type() != BSONType::Object) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '"
                                     << JSONSchemaParser::kSchemaPatternPropertiesKeyword
                                     << "' must be an object")};
    }

    for (auto&& patternSchema : patternPropertiesElt.embeddedObject()) {
        if (patternSchema.type() != BSONType::Object) {
            return {Status(ErrorCodes::TypeMismatch,
                           str::stream()
                               << "$jsonSchema keyword '"
                               << JSONSchemaParser::kSchemaPatternPropertiesKeyword
                               << "' has property '" << patternSchema.fieldNameStringData()
                               << "' which is not an object")};
        }

        // Parse the nested schema using a placeholder as the path, since we intend on using the
        // resulting match expression inside an ExpressionWithPlaceholder.
        auto nestedSchemaMatch =
            _parse(expCtx, kNamePlaceholder, patternSchema.embeddedObject(), ignoreUnknownKeywords);
        if (!nestedSchemaMatch.isOK()) {
            return nestedSchemaMatch.getStatus();
        }

        auto exprWithPlaceholder = std::make_unique<ExpressionWithPlaceholder>(
            kNamePlaceholder.toString(), std::move(nestedSchemaMatch.getValue()));
        Pattern pattern{patternSchema.fieldNameStringData()};
        patternProperties.emplace_back(std::move(pattern), std::move(exprWithPlaceholder));
    }

    return {std::move(patternProperties)};
}

StatusWithMatchExpression parseAdditionalProperties(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement additionalPropertiesElt,
    bool ignoreUnknownKeywords) {
    if (!additionalPropertiesElt) {
        // The absence of the 'additionalProperties' keyword is identical in meaning to the presence
        // of 'additionalProperties' with a value of true.
        return {std::make_unique<AlwaysTrueMatchExpression>()};
    }

    if (additionalPropertiesElt.type() != BSONType::Bool &&
        additionalPropertiesElt.type() != BSONType::Object) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '"
                                     << JSONSchemaParser::kSchemaAdditionalPropertiesKeyword
                                     << "' must be an object or a boolean")};
    }

    if (additionalPropertiesElt.type() == BSONType::Bool) {
        if (additionalPropertiesElt.boolean()) {
            return {std::make_unique<AlwaysTrueMatchExpression>()};
        } else {
            return {std::make_unique<AlwaysFalseMatchExpression>()};
        }
    }

    // Parse the nested schema using a placeholder as the path, since we intend on using the
    // resulting match expression inside an ExpressionWithPlaceholder.
    auto nestedSchemaMatch = _parse(
        expCtx, kNamePlaceholder, additionalPropertiesElt.embeddedObject(), ignoreUnknownKeywords);
    if (!nestedSchemaMatch.isOK()) {
        return nestedSchemaMatch.getStatus();
    }

    return {std::move(nestedSchemaMatch.getValue())};
}

/**
 * Returns a match expression which handles both the 'additionalProperties' and 'patternProperties'
 * keywords.
 */
StatusWithMatchExpression parseAllowedProperties(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    StringData path,
    BSONElement propertiesElt,
    BSONElement patternPropertiesElt,
    BSONElement additionalPropertiesElt,
    InternalSchemaTypeExpression* typeExpr,
    bool ignoreUnknownKeywords) {
    // Collect the set of properties named by the 'properties' keyword.
    StringDataSet propertyNames;
    if (propertiesElt) {
        std::vector<StringData> propertyNamesVec;
        for (auto&& elem : propertiesElt.embeddedObject()) {
            propertyNamesVec.push_back(elem.fieldNameStringData());
        }
        propertyNames.insert(propertyNamesVec.begin(), propertyNamesVec.end());
    }

    auto patternProperties =
        parsePatternProperties(expCtx, patternPropertiesElt, ignoreUnknownKeywords);
    if (!patternProperties.isOK()) {
        return patternProperties.getStatus();
    }

    auto otherwiseExpr =
        parseAdditionalProperties(expCtx, additionalPropertiesElt, ignoreUnknownKeywords);
    if (!otherwiseExpr.isOK()) {
        return otherwiseExpr.getStatus();
    }
    auto otherwiseWithPlaceholder = std::make_unique<ExpressionWithPlaceholder>(
        kNamePlaceholder.toString(), std::move(otherwiseExpr.getValue()));

    auto allowedPropertiesExpr = std::make_unique<InternalSchemaAllowedPropertiesMatchExpression>(
        std::move(propertyNames),
        kNamePlaceholder,
        std::move(patternProperties.getValue()),
        std::move(otherwiseWithPlaceholder));

    // If this is a top-level schema, then we have no path and there is no need for an explicit
    // object match node.
    if (path.empty()) {
        return {std::move(allowedPropertiesExpr)};
    }

    auto objectMatch = std::make_unique<InternalSchemaObjectMatchExpression>(
        path, std::move(allowedPropertiesExpr));

    return makeRestriction(BSONType::Object, path, std::move(objectMatch), typeExpr);
}

/**
 * Parses 'minProperties' and 'maxProperties' JSON Schema keywords.
 */
template <class T>
StatusWithMatchExpression parseNumProperties(StringData path,
                                             BSONElement numProperties,
                                             InternalSchemaTypeExpression* typeExpr) {
    auto parsedNumProps = numProperties.parseIntegerElementToNonNegativeLong();
    if (!parsedNumProps.isOK()) {
        return parsedNumProps.getStatus();
    }

    auto expr = std::make_unique<T>(parsedNumProps.getValue());

    if (path.empty()) {
        // This is a top-level schema.
        return {std::move(expr)};
    }

    auto objectMatch = std::make_unique<InternalSchemaObjectMatchExpression>(path, std::move(expr));

    return makeRestriction(BSONType::Object, path, std::move(objectMatch), typeExpr);
}

StatusWithMatchExpression makeDependencyExistsClause(StringData path, StringData dependencyName) {
    auto existsExpr = std::make_unique<ExistsMatchExpression>(dependencyName);

    if (path.empty()) {
        return {std::move(existsExpr)};
    }

    auto objectMatch =
        std::make_unique<InternalSchemaObjectMatchExpression>(path, std::move(existsExpr));

    return {std::move(objectMatch)};
}

StatusWithMatchExpression translateSchemaDependency(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    StringData path,
    BSONElement dependency,
    bool ignoreUnknownKeywords) {
    invariant(dependency.type() == BSONType::Object);

    auto nestedSchemaMatch =
        _parse(expCtx, path, dependency.embeddedObject(), ignoreUnknownKeywords);
    if (!nestedSchemaMatch.isOK()) {
        return nestedSchemaMatch.getStatus();
    }

    auto ifClause = makeDependencyExistsClause(path, dependency.fieldNameStringData());
    if (!ifClause.isOK()) {
        return ifClause.getStatus();
    }

    std::array<std::unique_ptr<MatchExpression>, 3> expressions = {
        std::move(ifClause.getValue()),
        std::move(nestedSchemaMatch.getValue()),
        std::make_unique<AlwaysTrueMatchExpression>()};

    auto condExpr = std::make_unique<InternalSchemaCondMatchExpression>(std::move(expressions));
    return {std::move(condExpr)};
}

StatusWithMatchExpression translatePropertyDependency(StringData path, BSONElement dependency) {
    invariant(dependency.type() == BSONType::Array);

    if (dependency.embeddedObject().isEmpty()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "property '" << dependency.fieldNameStringData()
                              << "' in $jsonSchema keyword '"
                              << JSONSchemaParser::kSchemaDependenciesKeyword
                              << "' cannot be an empty array"};
    }

    auto propertyDependencyExpr = std::make_unique<AndMatchExpression>();
    std::set<StringData> propertyDependencyNames;
    for (auto&& propertyDependency : dependency.embeddedObject()) {
        if (propertyDependency.type() != BSONType::String) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "array '" << dependency.fieldNameStringData()
                                  << "' in $jsonSchema keyword '"
                                  << JSONSchemaParser::kSchemaDependenciesKeyword
                                  << "' can only contain strings, but found element of type: "
                                  << typeName(propertyDependency.type())};
        }

        auto insertionResult = propertyDependencyNames.insert(propertyDependency.valueStringData());
        if (!insertionResult.second) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "array '" << dependency.fieldNameStringData()
                                  << "' in $jsonSchema keyword '"
                                  << JSONSchemaParser::kSchemaDependenciesKeyword
                                  << "' contains duplicate element: "
                                  << propertyDependency.valueStringData()};
        }

        auto propertyExistsExpr =
            makeDependencyExistsClause(path, propertyDependency.valueStringData());
        if (!propertyExistsExpr.isOK()) {
            return propertyExistsExpr.getStatus();
        }

        propertyDependencyExpr->add(propertyExistsExpr.getValue().release());
    }

    auto ifClause = makeDependencyExistsClause(path, dependency.fieldNameStringData());
    if (!ifClause.isOK()) {
        return ifClause.getStatus();
    }

    std::array<std::unique_ptr<MatchExpression>, 3> expressions = {
        {std::move(ifClause.getValue()),
         std::move(propertyDependencyExpr),
         std::make_unique<AlwaysTrueMatchExpression>()}};

    auto condExpr = std::make_unique<InternalSchemaCondMatchExpression>(std::move(expressions));
    return {std::move(condExpr)};
}

StatusWithMatchExpression parseDependencies(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            StringData path,
                                            BSONElement dependencies,
                                            bool ignoreUnknownKeywords) {
    if (dependencies.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '"
                              << JSONSchemaParser::kSchemaDependenciesKeyword
                              << "' must be an object"};
    }

    auto andExpr = std::make_unique<AndMatchExpression>();
    for (auto&& dependency : dependencies.embeddedObject()) {
        if (dependency.type() != BSONType::Object && dependency.type() != BSONType::Array) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "property '" << dependency.fieldNameStringData()
                                  << "' in $jsonSchema keyword '"
                                  << JSONSchemaParser::kSchemaDependenciesKeyword
                                  << "' must be either an object or an array"};
        }

        auto dependencyExpr = (dependency.type() == BSONType::Object)
            ? translateSchemaDependency(expCtx, path, dependency, ignoreUnknownKeywords)
            : translatePropertyDependency(path, dependency);
        if (!dependencyExpr.isOK()) {
            return dependencyExpr.getStatus();
        }

        andExpr->add(dependencyExpr.getValue().release());
    }

    return {std::move(andExpr)};
}

StatusWithMatchExpression parseUniqueItems(BSONElement uniqueItemsElt,
                                           StringData path,
                                           InternalSchemaTypeExpression* typeExpr) {
    if (!uniqueItemsElt.isBoolean()) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '"
                              << JSONSchemaParser::kSchemaUniqueItemsKeyword
                              << "' must be a boolean"};
    } else if (path.empty()) {
        return {std::make_unique<AlwaysTrueMatchExpression>()};
    } else if (uniqueItemsElt.boolean()) {
        auto uniqueItemsExpr = std::make_unique<InternalSchemaUniqueItemsMatchExpression>(path);
        return makeRestriction(BSONType::Array, path, std::move(uniqueItemsExpr), typeExpr);
    }

    return {std::make_unique<AlwaysTrueMatchExpression>()};
}

/**
 * Parses 'itemsElt' into a match expression and adds it to 'andExpr'. On success, returns the index
 * from which the "additionalItems" schema should be enforced, if needed.
 */
StatusWith<boost::optional<long long>> parseItems(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    StringData path,
    BSONElement itemsElt,
    bool ignoreUnknownKeywords,
    InternalSchemaTypeExpression* typeExpr,
    AndMatchExpression* andExpr) {
    boost::optional<long long> startIndexForAdditionalItems;
    if (itemsElt.type() == BSONType::Array) {
        // When "items" is an array, generate match expressions for each subschema for each position
        // in the array, which are bundled together in an AndMatchExpression.
        auto andExprForSubschemas = std::make_unique<AndMatchExpression>();
        auto index = 0LL;
        for (auto subschema : itemsElt.embeddedObject()) {
            if (subschema.type() != BSONType::Object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaItemsKeyword
                            << "' requires that each element of the array is an "
                               "object, but found a "
                            << subschema.type()};
            }

            // We want to make an ExpressionWithPlaceholder for $_internalSchemaMatchArrayIndex,
            // so we use our default placeholder as the path.
            auto parsedSubschema =
                _parse(expCtx, kNamePlaceholder, subschema.embeddedObject(), ignoreUnknownKeywords);
            if (!parsedSubschema.isOK()) {
                return parsedSubschema.getStatus();
            }
            auto exprWithPlaceholder = std::make_unique<ExpressionWithPlaceholder>(
                kNamePlaceholder.toString(), std::move(parsedSubschema.getValue()));
            auto matchArrayIndex = std::make_unique<InternalSchemaMatchArrayIndexMatchExpression>(
                path, index, std::move(exprWithPlaceholder));
            andExprForSubschemas->add(matchArrayIndex.release());
            ++index;
        }
        startIndexForAdditionalItems = index;

        if (path.empty()) {
            andExpr->add(std::make_unique<AlwaysTrueMatchExpression>().release());
        } else {
            andExpr->add(
                makeRestriction(BSONType::Array, path, std::move(andExprForSubschemas), typeExpr)
                    .release());
        }
    } else if (itemsElt.type() == BSONType::Object) {
        // When "items" is an object, generate a single AllElemMatchFromIndex that applies to every
        // element in the array to match. The parsed expression is intended for an
        // ExpressionWithPlaceholder, so we use the default placeholder as the path.
        auto nestedItemsSchema =
            _parse(expCtx, kNamePlaceholder, itemsElt.embeddedObject(), ignoreUnknownKeywords);
        if (!nestedItemsSchema.isOK()) {
            return nestedItemsSchema.getStatus();
        }
        auto exprWithPlaceholder = std::make_unique<ExpressionWithPlaceholder>(
            kNamePlaceholder.toString(), std::move(nestedItemsSchema.getValue()));

        if (path.empty()) {
            andExpr->add(std::make_unique<AlwaysTrueMatchExpression>().release());
        } else {
            constexpr auto startIndexForItems = 0LL;
            auto allElemMatch =
                std::make_unique<InternalSchemaAllElemMatchFromIndexMatchExpression>(
                    path, startIndexForItems, std::move(exprWithPlaceholder));
            andExpr->add(makeRestriction(BSONType::Array, path, std::move(allElemMatch), typeExpr)
                             .release());
        }
    } else {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaItemsKeyword
                              << "' must be an array or an object, not " << itemsElt.type()};
    }

    return startIndexForAdditionalItems;
}

Status parseAdditionalItems(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            StringData path,
                            BSONElement additionalItemsElt,
                            boost::optional<long long> startIndexForAdditionalItems,
                            bool ignoreUnknownKeywords,
                            InternalSchemaTypeExpression* typeExpr,
                            AndMatchExpression* andExpr) {
    std::unique_ptr<ExpressionWithPlaceholder> otherwiseExpr;
    if (additionalItemsElt.type() == BSONType::Bool) {
        const auto emptyPlaceholder = boost::none;
        if (additionalItemsElt.boolean()) {
            otherwiseExpr = std::make_unique<ExpressionWithPlaceholder>(
                emptyPlaceholder, std::make_unique<AlwaysTrueMatchExpression>());
        } else {
            otherwiseExpr = std::make_unique<ExpressionWithPlaceholder>(
                emptyPlaceholder, std::make_unique<AlwaysFalseMatchExpression>());
        }
    } else if (additionalItemsElt.type() == BSONType::Object) {
        auto parsedOtherwiseExpr = _parse(
            expCtx, kNamePlaceholder, additionalItemsElt.embeddedObject(), ignoreUnknownKeywords);
        if (!parsedOtherwiseExpr.isOK()) {
            return parsedOtherwiseExpr.getStatus();
        }
        otherwiseExpr = std::make_unique<ExpressionWithPlaceholder>(
            kNamePlaceholder.toString(), std::move(parsedOtherwiseExpr.getValue()));
    } else {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '"
                              << JSONSchemaParser::kSchemaAdditionalItemsKeyword
                              << "' must be either an object or a boolean, but got a "
                              << additionalItemsElt.type()};
    }

    // Only generate a match expression if needed.
    if (startIndexForAdditionalItems) {
        if (path.empty()) {
            andExpr->add(std::make_unique<AlwaysTrueMatchExpression>().release());
        } else {
            auto allElemMatch =
                std::make_unique<InternalSchemaAllElemMatchFromIndexMatchExpression>(
                    path, *startIndexForAdditionalItems, std::move(otherwiseExpr));
            andExpr->add(makeRestriction(BSONType::Array, path, std::move(allElemMatch), typeExpr)
                             .release());
        }
    }
    return Status::OK();
}

Status parseItemsAndAdditionalItems(StringMap<BSONElement>& keywordMap,
                                    const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    StringData path,
                                    bool ignoreUnknownKeywords,
                                    InternalSchemaTypeExpression* typeExpr,
                                    AndMatchExpression* andExpr) {
    boost::optional<long long> startIndexForAdditionalItems;
    if (auto itemsElt = keywordMap[JSONSchemaParser::kSchemaItemsKeyword]) {
        auto index = parseItems(expCtx, path, itemsElt, ignoreUnknownKeywords, typeExpr, andExpr);
        if (!index.isOK()) {
            return index.getStatus();
        }
        startIndexForAdditionalItems = index.getValue();
    }

    if (auto additionalItemsElt = keywordMap[JSONSchemaParser::kSchemaAdditionalItemsKeyword]) {
        return parseAdditionalItems(expCtx,
                                    path,
                                    additionalItemsElt,
                                    startIndexForAdditionalItems,
                                    ignoreUnknownKeywords,
                                    typeExpr,
                                    andExpr);
    }
    return Status::OK();
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
Status translateLogicalKeywords(StringMap<BSONElement>& keywordMap,
                                const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                StringData path,
                                AndMatchExpression* andExpr,
                                bool ignoreUnknownKeywords) {
    if (auto allOfElt = keywordMap[JSONSchemaParser::kSchemaAllOfKeyword]) {
        auto allOfExpr =
            parseLogicalKeyword<AndMatchExpression>(expCtx, path, allOfElt, ignoreUnknownKeywords);
        if (!allOfExpr.isOK()) {
            return allOfExpr.getStatus();
        }
        andExpr->add(allOfExpr.getValue().release());
    }

    if (auto anyOfElt = keywordMap[JSONSchemaParser::kSchemaAnyOfKeyword]) {
        auto anyOfExpr =
            parseLogicalKeyword<OrMatchExpression>(expCtx, path, anyOfElt, ignoreUnknownKeywords);
        if (!anyOfExpr.isOK()) {
            return anyOfExpr.getStatus();
        }
        andExpr->add(anyOfExpr.getValue().release());
    }

    if (auto oneOfElt = keywordMap[JSONSchemaParser::kSchemaOneOfKeyword]) {
        auto oneOfExpr = parseLogicalKeyword<InternalSchemaXorMatchExpression>(
            expCtx, path, oneOfElt, ignoreUnknownKeywords);
        if (!oneOfExpr.isOK()) {
            return oneOfExpr.getStatus();
        }
        andExpr->add(oneOfExpr.getValue().release());
    }

    if (auto notElt = keywordMap[JSONSchemaParser::kSchemaNotKeyword]) {
        if (notElt.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaNotKeyword
                                  << "' must be an object, but found an element of type "
                                  << notElt.type()};
        }

        auto parsedExpr = _parse(expCtx, path, notElt.embeddedObject(), ignoreUnknownKeywords);
        if (!parsedExpr.isOK()) {
            return parsedExpr.getStatus();
        }

        auto notMatchExpr = std::make_unique<NotMatchExpression>(parsedExpr.getValue().release());
        andExpr->add(notMatchExpr.release());
    }

    if (auto enumElt = keywordMap[JSONSchemaParser::kSchemaEnumKeyword]) {
        auto enumExpr = parseEnum(path, enumElt);
        if (!enumExpr.isOK()) {
            return enumExpr.getStatus();
        }
        andExpr->add(enumExpr.getValue().release());
    }

    return Status::OK();
}

/**
 * Parses JSON Schema array keywords in 'keywordMap' and adds them to 'andExpr'. Returns a non-OK
 * status if an error occurs during parsing.
 *
 * This function parses the following keywords:
 *  - minItems
 *  - maxItems
 *  - uniqueItems
 *  - items
 *  - additionalItems
 */
Status translateArrayKeywords(StringMap<BSONElement>& keywordMap,
                              const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              StringData path,
                              bool ignoreUnknownKeywords,
                              InternalSchemaTypeExpression* typeExpr,
                              AndMatchExpression* andExpr) {
    if (auto minItemsElt = keywordMap[JSONSchemaParser::kSchemaMinItemsKeyword]) {
        auto minItemsExpr = parseLength<InternalSchemaMinItemsMatchExpression>(
            path, minItemsElt, typeExpr, BSONType::Array);
        if (!minItemsExpr.isOK()) {
            return minItemsExpr.getStatus();
        }
        andExpr->add(minItemsExpr.getValue().release());
    }

    if (auto maxItemsElt = keywordMap[JSONSchemaParser::kSchemaMaxItemsKeyword]) {
        auto maxItemsExpr = parseLength<InternalSchemaMaxItemsMatchExpression>(
            path, maxItemsElt, typeExpr, BSONType::Array);
        if (!maxItemsExpr.isOK()) {
            return maxItemsExpr.getStatus();
        }
        andExpr->add(maxItemsExpr.getValue().release());
    }

    if (auto uniqueItemsElt = keywordMap[JSONSchemaParser::kSchemaUniqueItemsKeyword]) {
        auto uniqueItemsExpr = parseUniqueItems(uniqueItemsElt, path, typeExpr);
        if (!uniqueItemsExpr.isOK()) {
            return uniqueItemsExpr.getStatus();
        }
        andExpr->add(uniqueItemsExpr.getValue().release());
    }

    return parseItemsAndAdditionalItems(
        keywordMap, expCtx, path, ignoreUnknownKeywords, typeExpr, andExpr);
}

/**
 * Parses JSON Schema keywords related to objects in 'keywordMap' and adds them to 'andExpr'.
 * Returns a non-OK status if an error occurs during parsing.
 *
 * This function parses the following keywords:
 *  - additionalProperties
 *  - dependencies
 *  - maxProperties
 *  - minProperties
 *  - patternProperties
 *  - properties
 *  - required
 */
Status translateObjectKeywords(StringMap<BSONElement>& keywordMap,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               StringData path,
                               InternalSchemaTypeExpression* typeExpr,
                               AndMatchExpression* andExpr,
                               bool ignoreUnknownKeywords) {
    StringDataSet requiredProperties;
    if (auto requiredElt = keywordMap[JSONSchemaParser::kSchemaRequiredKeyword]) {
        auto requiredStatus = parseRequired(requiredElt);
        if (!requiredStatus.isOK()) {
            return requiredStatus.getStatus();
        }
        requiredProperties = std::move(requiredStatus.getValue());
    }

    if (auto propertiesElt = keywordMap[JSONSchemaParser::kSchemaPropertiesKeyword]) {
        auto propertiesExpr = parseProperties(
            expCtx, path, propertiesElt, typeExpr, requiredProperties, ignoreUnknownKeywords);
        if (!propertiesExpr.isOK()) {
            return propertiesExpr.getStatus();
        }
        andExpr->add(propertiesExpr.getValue().release());
    }

    {
        auto propertiesElt = keywordMap[JSONSchemaParser::kSchemaPropertiesKeyword];
        auto patternPropertiesElt = keywordMap[JSONSchemaParser::kSchemaPatternPropertiesKeyword];
        auto additionalPropertiesElt =
            keywordMap[JSONSchemaParser::kSchemaAdditionalPropertiesKeyword];

        if (patternPropertiesElt || additionalPropertiesElt) {
            auto allowedPropertiesExpr = parseAllowedProperties(expCtx,
                                                                path,
                                                                propertiesElt,
                                                                patternPropertiesElt,
                                                                additionalPropertiesElt,
                                                                typeExpr,
                                                                ignoreUnknownKeywords);
            if (!allowedPropertiesExpr.isOK()) {
                return allowedPropertiesExpr.getStatus();
            }
            andExpr->add(allowedPropertiesExpr.getValue().release());
        }
    }

    if (!requiredProperties.empty()) {
        auto requiredExpr = translateRequired(requiredProperties, path, typeExpr);
        if (!requiredExpr.isOK()) {
            return requiredExpr.getStatus();
        }
        andExpr->add(requiredExpr.getValue().release());
    }

    if (auto minPropertiesElt = keywordMap[JSONSchemaParser::kSchemaMinPropertiesKeyword]) {
        auto minPropExpr = parseNumProperties<InternalSchemaMinPropertiesMatchExpression>(
            path, minPropertiesElt, typeExpr);
        if (!minPropExpr.isOK()) {
            return minPropExpr.getStatus();
        }
        andExpr->add(minPropExpr.getValue().release());
    }

    if (auto maxPropertiesElt = keywordMap[JSONSchemaParser::kSchemaMaxPropertiesKeyword]) {
        auto maxPropExpr = parseNumProperties<InternalSchemaMaxPropertiesMatchExpression>(
            path, maxPropertiesElt, typeExpr);
        if (!maxPropExpr.isOK()) {
            return maxPropExpr.getStatus();
        }
        andExpr->add(maxPropExpr.getValue().release());
    }

    if (auto dependenciesElt = keywordMap[JSONSchemaParser::kSchemaDependenciesKeyword]) {
        auto dependenciesExpr =
            parseDependencies(expCtx, path, dependenciesElt, ignoreUnknownKeywords);
        if (!dependenciesExpr.isOK()) {
            return dependenciesExpr.getStatus();
        }
        andExpr->add(dependenciesExpr.getValue().release());
    }

    return Status::OK();
}

/**
 * Parses JSON Schema scalar keywords in 'keywordMap' and adds them to 'andExpr'. Returns a non-OK
 * status if an error occurs during parsing.
 *
 * This function parses the following keywords:
 *  - minimum
 *  - exclusiveMinimum
 *  - maximum
 *  - exclusiveMaximum
 *  - minLength
 *  - maxLength
 *  - pattern
 *  - multipleOf
 */
Status translateScalarKeywords(StringMap<BSONElement>& keywordMap,
                               StringData path,
                               InternalSchemaTypeExpression* typeExpr,
                               AndMatchExpression* andExpr) {
    // String keywords.
    if (auto patternElt = keywordMap[JSONSchemaParser::kSchemaPatternKeyword]) {
        auto patternExpr = parsePattern(path, patternElt, typeExpr);
        if (!patternExpr.isOK()) {
            return patternExpr.getStatus();
        }
        andExpr->add(patternExpr.getValue().release());
    }

    if (auto maxLengthElt = keywordMap[JSONSchemaParser::kSchemaMaxLengthKeyword]) {
        auto maxLengthExpr = parseLength<InternalSchemaMaxLengthMatchExpression>(
            path, maxLengthElt, typeExpr, BSONType::String);
        if (!maxLengthExpr.isOK()) {
            return maxLengthExpr.getStatus();
        }
        andExpr->add(maxLengthExpr.getValue().release());
    }

    if (auto minLengthElt = keywordMap[JSONSchemaParser::kSchemaMinLengthKeyword]) {
        auto minLengthExpr = parseLength<InternalSchemaMinLengthMatchExpression>(
            path, minLengthElt, typeExpr, BSONType::String);
        if (!minLengthExpr.isOK()) {
            return minLengthExpr.getStatus();
        }
        andExpr->add(minLengthExpr.getValue().release());
    }

    // Numeric keywords.
    if (auto multipleOfElt = keywordMap[JSONSchemaParser::kSchemaMultipleOfKeyword]) {
        auto multipleOfExpr = parseMultipleOf(path, multipleOfElt, typeExpr);
        if (!multipleOfExpr.isOK()) {
            return multipleOfExpr.getStatus();
        }
        andExpr->add(multipleOfExpr.getValue().release());
    }

    if (auto maximumElt = keywordMap[JSONSchemaParser::kSchemaMaximumKeyword]) {
        bool isExclusiveMaximum = false;
        if (auto exclusiveMaximumElt =
                keywordMap[JSONSchemaParser::kSchemaExclusiveMaximumKeyword]) {
            if (!exclusiveMaximumElt.isBoolean()) {
                return {Status(ErrorCodes::TypeMismatch,
                               str::stream() << "$jsonSchema keyword '"
                                             << JSONSchemaParser::kSchemaExclusiveMaximumKeyword
                                             << "' must be a boolean")};
            } else {
                isExclusiveMaximum = exclusiveMaximumElt.boolean();
            }
        }
        auto maxExpr = parseMaximum(path, maximumElt, typeExpr, isExclusiveMaximum);
        if (!maxExpr.isOK()) {
            return maxExpr.getStatus();
        }
        andExpr->add(maxExpr.getValue().release());
    } else if (keywordMap[JSONSchemaParser::kSchemaExclusiveMaximumKeyword]) {
        // If "exclusiveMaximum" is present, "maximum" must also be present.
        return {ErrorCodes::FailedToParse,
                str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaMaximumKeyword
                              << "' must be a present if "
                              << JSONSchemaParser::kSchemaExclusiveMaximumKeyword << " is present"};
    }

    if (auto minimumElt = keywordMap[JSONSchemaParser::kSchemaMinimumKeyword]) {
        bool isExclusiveMinimum = false;
        if (auto exclusiveMinimumElt =
                keywordMap[JSONSchemaParser::kSchemaExclusiveMinimumKeyword]) {
            if (!exclusiveMinimumElt.isBoolean()) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "$jsonSchema keyword '"
                                      << JSONSchemaParser::kSchemaExclusiveMinimumKeyword
                                      << "' must be a boolean"};
            } else {
                isExclusiveMinimum = exclusiveMinimumElt.boolean();
            }
        }
        auto minExpr = parseMinimum(path, minimumElt, typeExpr, isExclusiveMinimum);
        if (!minExpr.isOK()) {
            return minExpr.getStatus();
        }
        andExpr->add(minExpr.getValue().release());
    } else if (keywordMap[JSONSchemaParser::kSchemaExclusiveMinimumKeyword]) {
        // If "exclusiveMinimum" is present, "minimum" must also be present.
        return {ErrorCodes::FailedToParse,
                str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaMinimumKeyword
                              << "' must be a present if "
                              << JSONSchemaParser::kSchemaExclusiveMinimumKeyword << " is present"};
    }

    return Status::OK();
}

/**
 * Parses JSON Schema encrypt keyword in 'keywordMap' and adds it to 'andExpr'. Returns a
 * non-OK status if an error occurs during parsing.
 */
Status translateEncryptionKeywords(StringMap<BSONElement>& keywordMap,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   StringData path,
                                   AndMatchExpression* andExpr) {
    auto encryptElt = keywordMap[JSONSchemaParser::kSchemaEncryptKeyword];
    auto encryptMetadataElt = keywordMap[JSONSchemaParser::kSchemaEncryptMetadataKeyword];

    if (encryptElt && encryptMetadataElt) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Cannot specify both $jsonSchema keywords '"
                                    << JSONSchemaParser::kSchemaEncryptKeyword << "' and '"
                                    << JSONSchemaParser::kSchemaEncryptMetadataKeyword << "'");
    }

    if (encryptMetadataElt) {
        if (encryptMetadataElt.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '"
                                  << JSONSchemaParser::kSchemaEncryptMetadataKeyword
                                  << "' must be an object "};
        } else if (encryptMetadataElt.embeddedObject().isEmpty()) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "$jsonSchema keyword '"
                                  << JSONSchemaParser::kSchemaEncryptMetadataKeyword
                                  << "' cannot be an empty object "};
        }

        const IDLParserErrorContext ctxt("encryptMetadata");
        try {
            // Discard the result as we are only concerned with validation.
            EncryptionMetadata::parse(ctxt, encryptMetadataElt.embeddedObject());
        } catch (const AssertionException&) {
            return exceptionToStatus();
        }
    }

    if (encryptElt) {
        if (encryptElt.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '"
                                  << JSONSchemaParser::kSchemaEncryptKeyword
                                  << "' must be an object "};
        }

        try {
            // This checks the types of all the fields. Will throw on any parsing error.
            const IDLParserErrorContext encryptCtxt("encrypt");
            auto encryptInfo = EncryptionInfo::parse(encryptCtxt, encryptElt.embeddedObject());
            auto infoType = encryptInfo.getBsonType();

            andExpr->add(new InternalSchemaBinDataSubTypeExpression(path, BinDataType::Encrypt));

            if (auto typeOptional = infoType)
                andExpr->add(new InternalSchemaBinDataEncryptedTypeExpression(
                    path, typeOptional->typeSet()));
        } catch (const AssertionException&) {
            return exceptionToStatus();
        }
    }

    return Status::OK();
}

/**
 * Validates that the following metadata keywords have the correct type:
 *  - description
 *  - title
 */
Status validateMetadataKeywords(StringMap<BSONElement>& keywordMap) {
    if (auto descriptionElem = keywordMap[JSONSchemaParser::kSchemaDescriptionKeyword]) {
        if (descriptionElem.type() != BSONType::String) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "$jsonSchema keyword '"
                                        << JSONSchemaParser::kSchemaDescriptionKeyword
                                        << "' must be of type string");
        }
    }

    if (auto titleElem = keywordMap[JSONSchemaParser::kSchemaTitleKeyword]) {
        if (titleElem.type() != BSONType::String) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream()
                              << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaTitleKeyword
                              << "' must be of type string");
        }
    }
    return Status::OK();
}

StatusWithMatchExpression _parse(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 StringData path,
                                 BSONObj schema,
                                 bool ignoreUnknownKeywords) {
    // Map from JSON Schema keyword to the corresponding element from 'schema', or to an empty
    // BSONElement if the JSON Schema keyword is not specified.
    StringMap<BSONElement> keywordMap{
        {std::string(JSONSchemaParser::kSchemaAdditionalItemsKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaAdditionalPropertiesKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaAllOfKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaAnyOfKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaBsonTypeKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaDependenciesKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaDescriptionKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaEncryptKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaEncryptMetadataKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaEnumKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaExclusiveMaximumKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaExclusiveMinimumKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaItemsKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaMaxItemsKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaMaxLengthKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaMaxPropertiesKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaMaximumKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaMinItemsKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaMinLengthKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaMinPropertiesKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaMinimumKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaMultipleOfKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaNotKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaOneOfKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaPatternKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaPatternPropertiesKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaPropertiesKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaRequiredKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaTitleKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaTypeKeyword), {}},
        {std::string(JSONSchemaParser::kSchemaUniqueItemsKeyword), {}},
    };

    for (auto&& elt : schema) {
        auto it = keywordMap.find(elt.fieldNameStringData());
        if (it == keywordMap.end()) {
            if (unsupportedKeywords.find(elt.fieldNameStringData()) != unsupportedKeywords.end()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "$jsonSchema keyword '" << elt.fieldNameStringData()
                                            << "' is not currently supported");
            } else if (!ignoreUnknownKeywords) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << "Unknown $jsonSchema keyword: " << elt.fieldNameStringData());
            }
            continue;
        }

        if (it->second) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Duplicate $jsonSchema keyword: " << elt.fieldNameStringData());
        }

        keywordMap[elt.fieldNameStringData()] = elt;
    }

    auto metadataStatus = validateMetadataKeywords(keywordMap);
    if (!metadataStatus.isOK()) {
        return metadataStatus;
    }

    auto typeElem = keywordMap[JSONSchemaParser::kSchemaTypeKeyword];
    auto bsonTypeElem = keywordMap[JSONSchemaParser::kSchemaBsonTypeKeyword];
    auto encryptElem = keywordMap[JSONSchemaParser::kSchemaEncryptKeyword];
    if (typeElem && bsonTypeElem) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Cannot specify both $jsonSchema keywords '"
                                    << JSONSchemaParser::kSchemaTypeKeyword << "' and '"
                                    << JSONSchemaParser::kSchemaBsonTypeKeyword << "'");
    } else if (typeElem && encryptElem) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaEncryptKeyword
                          << "' cannot be used in conjunction with '"
                          << JSONSchemaParser::kSchemaTypeKeyword << "', '"
                          << JSONSchemaParser::kSchemaEncryptKeyword
                          << "' implies type 'bsonType::BinData'");
    } else if (bsonTypeElem && encryptElem) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaEncryptKeyword
                          << "' cannot be used in conjunction with '"
                          << JSONSchemaParser::kSchemaBsonTypeKeyword << "', '"
                          << JSONSchemaParser::kSchemaEncryptKeyword
                          << "' implies type 'bsonType::BinData'");
    }

    std::unique_ptr<InternalSchemaTypeExpression> typeExpr;
    if (typeElem) {
        auto parsed = parseType(path,
                                JSONSchemaParser::kSchemaTypeKeyword,
                                typeElem,
                                MatcherTypeSet::findJsonSchemaTypeAlias);
        if (!parsed.isOK()) {
            return parsed.getStatus();
        }
        typeExpr = std::move(parsed.getValue());
    } else if (bsonTypeElem) {
        auto parseBsonTypeResult = parseType(
            path, JSONSchemaParser::kSchemaBsonTypeKeyword, bsonTypeElem, findBSONTypeAlias);
        if (!parseBsonTypeResult.isOK()) {
            return parseBsonTypeResult.getStatus();
        }
        typeExpr = std::move(parseBsonTypeResult.getValue());
    } else if (encryptElem) {
        // The presence of the encrypt keyword implies the restriction that the field must be
        // of type BinData.
        typeExpr =
            std::make_unique<InternalSchemaTypeExpression>(path, MatcherTypeSet(BSONType::BinData));
    }

    auto andExpr = std::make_unique<AndMatchExpression>();

    auto translationStatus =
        translateScalarKeywords(keywordMap, path, typeExpr.get(), andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus = translateArrayKeywords(
        keywordMap, expCtx, path, ignoreUnknownKeywords, typeExpr.get(), andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus = translateEncryptionKeywords(keywordMap, expCtx, path, andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus = translateObjectKeywords(
        keywordMap, expCtx, path, typeExpr.get(), andExpr.get(), ignoreUnknownKeywords);
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus =
        translateLogicalKeywords(keywordMap, expCtx, path, andExpr.get(), ignoreUnknownKeywords);
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    if (path.empty() && typeExpr && !typeExpr->typeSet().hasType(BSONType::Object)) {
        // This is a top-level schema which requires that the type is something other than
        // "object". Since we only know how to store objects, this schema matches nothing.
        return {std::make_unique<AlwaysFalseMatchExpression>()};
    }

    if (!path.empty() && typeExpr) {
        andExpr->add(typeExpr.release());
    }
    return {std::move(andExpr)};
}
}  // namespace

StatusWith<MatcherTypeSet> JSONSchemaParser::parseTypeSet(
    BSONElement typeElt, const findBSONTypeAliasFun& aliasMapFind) {
    if (typeElt.type() != BSONType::String && typeElt.type() != BSONType::Array) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '" << typeElt.fieldNameStringData()
                                     << "' must be either a string or an array of strings")};
    }

    std::set<StringData> aliases;
    if (typeElt.type() == BSONType::String) {
        if (typeElt.valueStringData() == JSONSchemaParser::kSchemaTypeInteger) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "$jsonSchema type '" << JSONSchemaParser::kSchemaTypeInteger
                                  << "' is not currently supported."};
        }
        aliases.insert(typeElt.valueStringData());
    } else {
        for (auto&& typeArrayEntry : typeElt.embeddedObject()) {
            if (typeArrayEntry.type() != BSONType::String) {
                return {Status(ErrorCodes::TypeMismatch,
                               str::stream()
                                   << "$jsonSchema keyword '" << typeElt.fieldNameStringData()
                                   << "' array elements must be strings")};
            }

            if (typeArrayEntry.valueStringData() == JSONSchemaParser::kSchemaTypeInteger) {
                return {ErrorCodes::FailedToParse,
                        str::stream()
                            << "$jsonSchema type '" << JSONSchemaParser::kSchemaTypeInteger
                            << "' is not currently supported."};
            }

            auto insertionResult = aliases.insert(typeArrayEntry.valueStringData());
            if (!insertionResult.second) {
                return {
                    Status(ErrorCodes::FailedToParse,
                           str::stream()
                               << "$jsonSchema keyword '" << typeElt.fieldNameStringData()
                               << "' has duplicate value: " << typeArrayEntry.valueStringData())};
            }
        }
    }

    return MatcherTypeSet::fromStringAliases(std::move(aliases), aliasMapFind);
}

StatusWithMatchExpression JSONSchemaParser::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONObj schema,
    bool ignoreUnknownKeywords) {
    LOGV2_DEBUG(20728,
                5,
                "Parsing JSON Schema: {schema_jsonString_JsonStringFormat_LegacyStrict}",
                "schema_jsonString_JsonStringFormat_LegacyStrict"_attr =
                    schema.jsonString(JsonStringFormat::LegacyStrict));
    try {
        auto translation = _parse(expCtx, ""_sd, schema, ignoreUnknownKeywords);
        if (shouldLog(logv2::LogSeverity::Debug(5)) && translation.isOK()) {
            LOGV2_DEBUG(20729,
                        5,
                        "Translated schema match expression: {translation_getValue_debugString}",
                        "translation_getValue_debugString"_attr =
                            translation.getValue()->debugString());
        }
        return translation;
    } catch (const DBException& ex) {
        return {ex.toStatus()};
    }
}
}  // namespace mongo

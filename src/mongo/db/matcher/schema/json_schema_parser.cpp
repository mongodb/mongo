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

#include "mongo/db/matcher/schema/json_schema_parser.h"

#include <memory>

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/unordered_fields_bsonelement_comparator.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/matcher/doc_validation_util.h"
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using PatternSchema = InternalSchemaAllowedPropertiesMatchExpression::PatternSchema;
using Pattern = InternalSchemaAllowedPropertiesMatchExpression::Pattern;
using AllowedFeatureSet = MatchExpressionParser::AllowedFeatureSet;
using ErrorAnnotation = MatchExpression::ErrorAnnotation;
using AnnotationMode = ErrorAnnotation::Mode;

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

constexpr StringData kNamePlaceholder = JSONSchemaParser::kNamePlaceholder;

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
                                 AllowedFeatureSet allowedFeatures,
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
std::unique_ptr<MatchExpression> makeRestriction(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MatcherTypeSet& restrictionType,
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
            // from the type to which this restriction applies.
            return std::make_unique<AlwaysTrueMatchExpression>(
                doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));
        }
    }

    // Generate and return the following expression tree:
    //
    //  (OR (<restrictionExpr>) (NOT (INTERNAL_SCHEMA_TYPE <restrictionType>))
    //
    // We need to do this because restriction keywords do not apply when a field is either not
    // present or of a different type.
    auto typeExpr = std::make_unique<InternalSchemaTypeExpression>(
        path,
        restrictionType,
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));

    auto notExpr = std::make_unique<NotMatchExpression>(
        std::move(typeExpr),
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));

    auto orExpr = std::make_unique<OrMatchExpression>(
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));
    orExpr->add(std::move(notExpr));
    orExpr->add(std::move(restrictionExpr));

    return orExpr;
}

StatusWith<std::unique_ptr<InternalSchemaTypeExpression>> parseType(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
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

    auto typeExpr = std::make_unique<InternalSchemaTypeExpression>(
        path,
        std::move(typeSet.getValue()),
        doc_validation_error::createAnnotation(
            expCtx, typeElt.fieldNameStringData().toString(), typeElt.wrap()));

    return {std::move(typeExpr)};
}

StatusWithMatchExpression parseMaximum(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       StringData path,
                                       BSONElement maximum,
                                       InternalSchemaTypeExpression* typeExpr,
                                       bool isExclusiveMaximum) {
    if (!maximum.isNumber()) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaMaximumKeyword
                           << "' must be a number")};
    }

    clonable_ptr<ErrorAnnotation> annotation;
    if (isExclusiveMaximum) {
        annotation =
            doc_validation_error::createAnnotation(expCtx,
                                                   maximum.fieldNameStringData().toString(),
                                                   BSON(maximum << "exclusiveMaximum" << true));
    } else {
        annotation = doc_validation_error::createAnnotation(
            expCtx, maximum.fieldNameStringData().toString(), maximum.wrap());
    }

    if (path.empty()) {
        // This restriction has no effect in a top-level schema, since we only store objects.
        return {std::make_unique<AlwaysTrueMatchExpression>(std::move(annotation))};
    }

    std::unique_ptr<ComparisonMatchExpression> expr;
    if (isExclusiveMaximum) {
        expr = std::make_unique<LTMatchExpression>(path, maximum, std::move(annotation));
    } else {
        expr = std::make_unique<LTEMatchExpression>(path, maximum, std::move(annotation));
    }

    MatcherTypeSet restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(expCtx, restrictionType, path, std::move(expr), typeExpr);
}

StatusWithMatchExpression parseMinimum(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       StringData path,
                                       BSONElement minimum,
                                       InternalSchemaTypeExpression* typeExpr,
                                       bool isExclusiveMinimum) {
    if (!minimum.isNumber()) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaMinimumKeyword
                           << "' must be a number")};
    }

    clonable_ptr<ErrorAnnotation> annotation;
    if (isExclusiveMinimum) {
        annotation =
            doc_validation_error::createAnnotation(expCtx,
                                                   minimum.fieldNameStringData().toString(),
                                                   BSON(minimum << "exclusiveMinimum" << true));
    } else {
        annotation = doc_validation_error::createAnnotation(
            expCtx, minimum.fieldNameStringData().toString(), minimum.wrap());
    }

    if (path.empty()) {
        // This restriction has no effect in a top-level schema, since we only store objects.
        return {std::make_unique<AlwaysTrueMatchExpression>(std::move(annotation))};
    }

    std::unique_ptr<ComparisonMatchExpression> expr;
    if (isExclusiveMinimum) {
        expr = std::make_unique<GTMatchExpression>(path, minimum, std::move(annotation));
    } else {
        expr = std::make_unique<GTEMatchExpression>(path, minimum, std::move(annotation));
    }

    MatcherTypeSet restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(expCtx, restrictionType, path, std::move(expr), typeExpr);
}

/**
 * Parses length-related keywords that expect a nonnegative long as an argument.
 */
template <class T>
StatusWithMatchExpression parseLength(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      StringData path,
                                      BSONElement length,
                                      InternalSchemaTypeExpression* typeExpr,
                                      BSONType restrictionType) {
    auto parsedLength = length.parseIntegerElementToNonNegativeLong();
    if (!parsedLength.isOK()) {
        return parsedLength.getStatus();
    }

    auto annotation = doc_validation_error::createAnnotation(
        expCtx, length.fieldNameStringData().toString(), length.wrap());
    if (path.empty()) {
        return {std::make_unique<AlwaysTrueMatchExpression>(std::move(annotation))};
    }

    auto expr = std::make_unique<T>(path, parsedLength.getValue(), std::move(annotation));
    return makeRestriction(expCtx, restrictionType, path, std::move(expr), typeExpr);
}

StatusWithMatchExpression parsePattern(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       StringData path,
                                       BSONElement pattern,
                                       InternalSchemaTypeExpression* typeExpr) {
    if (pattern.type() != BSONType::String) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaPatternKeyword
                           << "' must be a string")};
    }

    auto annotation = doc_validation_error::createAnnotation(
        expCtx, pattern.fieldNameStringData().toString(), pattern.wrap());
    if (path.empty()) {
        return {std::make_unique<AlwaysTrueMatchExpression>(std::move(annotation))};
    }

    // JSON Schema does not allow regex flags to be specified.
    constexpr auto emptyFlags = "";
    auto expr = std::make_unique<RegexMatchExpression>(
        path, pattern.valueStringData(), emptyFlags, std::move(annotation));

    return makeRestriction(expCtx, BSONType::String, path, std::move(expr), typeExpr);
}

StatusWithMatchExpression parseMultipleOf(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          StringData path,
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
    auto annotation = doc_validation_error::createAnnotation(
        expCtx, multipleOf.fieldNameStringData().toString(), multipleOf.wrap());
    if (path.empty()) {
        return {std::make_unique<AlwaysTrueMatchExpression>(std::move(annotation))};
    }

    auto expr = std::make_unique<InternalSchemaFmodMatchExpression>(
        path, multipleOf.numberDecimal(), Decimal128(0), std::move(annotation));

    MatcherTypeSet restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(expCtx, restrictionType, path, std::move(expr), typeExpr);
}

template <class T>
StatusWithMatchExpression parseLogicalKeyword(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              StringData path,
                                              BSONElement logicalElement,
                                              AllowedFeatureSet allowedFeatures,
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

    std::unique_ptr<T> listOfExpr = std::make_unique<T>(doc_validation_error::createAnnotation(
        expCtx, logicalElement.fieldNameStringData().toString(), BSONObj()));
    for (const auto& elem : logicalElementObj) {
        if (elem.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '" << logicalElement.fieldNameStringData()
                                  << "' must be an array of objects, but found an element of type "
                                  << elem.type()};
        }

        auto nestedSchemaMatch =
            _parse(expCtx, path, elem.embeddedObject(), allowedFeatures, ignoreUnknownKeywords);
        if (!nestedSchemaMatch.isOK()) {
            return nestedSchemaMatch.getStatus();
        }

        listOfExpr->add(std::move(nestedSchemaMatch.getValue()));
    }

    return {std::move(listOfExpr)};
}

StatusWithMatchExpression parseEnum(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    StringData path,
                                    BSONElement enumElement) {
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

    auto orExpr = std::make_unique<OrMatchExpression>(doc_validation_error::createAnnotation(
        expCtx, enumElement.fieldNameStringData().toString(), enumElement.wrap()));
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
                    arrayElem.embeddedObject(),
                    doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));
                orExpr->add(std::move(rootDocEq));
            }
        } else {
            auto eqExpr = std::make_unique<InternalSchemaEqMatchExpression>(
                path,
                arrayElem,
                doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));
            orExpr->add(std::move(eqExpr));
        }
    }

    // Make sure that the OR expression has at least 1 child.
    if (orExpr->numChildren() == 0) {
        return {std::make_unique<AlwaysFalseMatchExpression>(doc_validation_error::createAnnotation(
            expCtx, enumElement.fieldNameStringData().toString(), enumElement.wrap()))};
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

    return properties;
}

/**
 * Given the already-parsed set of required properties, returns a MatchExpression which ensures that
 * those properties exist. Returns a parsing error if the translation fails.
 */
StatusWithMatchExpression translateRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            const StringDataSet& requiredProperties,
                                            BSONElement requiredElt,
                                            StringData path,
                                            InternalSchemaTypeExpression* typeExpr) {
    auto andExpr = std::make_unique<AndMatchExpression>(
        doc_validation_error::createAnnotation(expCtx, "required", requiredElt.wrap()));

    std::vector<StringData> sortedProperties(requiredProperties.begin(), requiredProperties.end());
    std::sort(sortedProperties.begin(), sortedProperties.end());
    for (auto&& propertyName : sortedProperties) {
        // This node is tagged as '_propertyExists' to indicate that it will produce a path instead
        // of a detailed BSONObj error during error generation.
        andExpr->add(std::make_unique<ExistsMatchExpression>(
            propertyName,
            doc_validation_error::createAnnotation(expCtx, "_propertyExists", BSONObj())));
    }

    // If this is a top-level schema, then we know that we are matching against objects, and there
    // is no need to worry about ensuring that non-objects match.
    if (path.empty()) {
        return {std::move(andExpr)};
    }

    auto objectMatch = std::make_unique<InternalSchemaObjectMatchExpression>(
        path,
        std::move(andExpr),
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));

    return makeRestriction(expCtx, BSONType::Object, path, std::move(objectMatch), typeExpr);
}

StatusWithMatchExpression parseProperties(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          StringData path,
                                          BSONElement propertiesElt,
                                          InternalSchemaTypeExpression* typeExpr,
                                          const StringDataSet& requiredProperties,
                                          AllowedFeatureSet allowedFeatures,
                                          bool ignoreUnknownKeywords) {
    if (propertiesElt.type() != BSONType::Object) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream()
                           << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaPropertiesKeyword
                           << "' must be an object")};
    }
    auto propertiesObj = propertiesElt.embeddedObject();

    auto andExpr = std::make_unique<AndMatchExpression>(doc_validation_error::createAnnotation(
        expCtx, propertiesElt.fieldNameStringData().toString(), BSONObj()));
    for (auto&& property : propertiesObj) {
        if (property.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Nested schema for $jsonSchema property '"
                                  << property.fieldNameStringData() << "' must be an object"};
        }

        auto nestedSchemaMatch = _parse(expCtx,
                                        property.fieldNameStringData(),
                                        property.embeddedObject(),
                                        allowedFeatures,
                                        ignoreUnknownKeywords);
        if (!nestedSchemaMatch.isOK()) {
            return nestedSchemaMatch.getStatus();
        }

        nestedSchemaMatch.getValue()->setErrorAnnotation(doc_validation_error::createAnnotation(
            expCtx,
            "_property",
            BSON("propertyName" << property.fieldNameStringData().toString()),
            property.Obj()));
        if (requiredProperties.find(property.fieldNameStringData()) != requiredProperties.end()) {
            // The field name for which we created the nested schema is a required property. This
            // property must exist and therefore must match 'nestedSchemaMatch'.
            andExpr->add(std::move(nestedSchemaMatch.getValue()));
        } else {
            // This property either must not exist or must match the nested schema. Therefore, we
            // generate the match expression (OR (NOT (EXISTS)) <nestedSchemaMatch>).
            auto existsExpr = std::make_unique<ExistsMatchExpression>(
                property.fieldNameStringData(),
                doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));

            auto notExpr = std::make_unique<NotMatchExpression>(
                std::move(existsExpr),
                doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));

            auto orExpr = std::make_unique<OrMatchExpression>(
                doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));
            orExpr->add(std::move(notExpr));
            orExpr->add(std::move(nestedSchemaMatch.getValue()));

            andExpr->add(std::move(orExpr));
        }
    }

    // If this is a top-level schema, then we have no path and there is no need for an
    // explicit object match node.
    if (path.empty()) {
        return {std::move(andExpr)};
    }

    auto objectMatch = std::make_unique<InternalSchemaObjectMatchExpression>(
        path,
        std::move(andExpr),
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));

    return makeRestriction(expCtx, BSONType::Object, path, std::move(objectMatch), typeExpr);
}

StatusWith<std::vector<PatternSchema>> parsePatternProperties(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement patternPropertiesElt,
    AllowedFeatureSet allowedFeatures,
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
        auto nestedSchemaMatch = _parse(expCtx,
                                        kNamePlaceholder,
                                        patternSchema.embeddedObject(),
                                        allowedFeatures,
                                        ignoreUnknownKeywords);
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
    AllowedFeatureSet allowedFeatures,
    bool ignoreUnknownKeywords,
    bool topLevelRequiredMissingID) {
    if (!additionalPropertiesElt) {
        // The absence of the 'additionalProperties' keyword is identical in meaning to the presence
        // of 'additionalProperties' with a value of true.
        return {std::make_unique<AlwaysTrueMatchExpression>(
            doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore))};
    }

    if (additionalPropertiesElt.type() != BSONType::Bool &&
        additionalPropertiesElt.type() != BSONType::Object) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '"
                                     << JSONSchemaParser::kSchemaAdditionalPropertiesKeyword
                                     << "' must be an object or a boolean")};
    }

    auto annotation = doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore);
    if (additionalPropertiesElt.type() == BSONType::Bool) {
        if (additionalPropertiesElt.boolean()) {
            return {std::make_unique<AlwaysTrueMatchExpression>(std::move(annotation))};
        } else {
            if (topLevelRequiredMissingID) {
                LOGV2_WARNING(3216000,
                              "$jsonSchema validator does not allow '_id' field. This validator "
                              "will reject all "
                              "documents, consider adding '_id' to the allowed fields.");
            }
            return {std::make_unique<AlwaysFalseMatchExpression>(std::move(annotation))};
        }
    }

    // Parse the nested schema using a placeholder as the path, since we intend on using the
    // resulting match expression inside an ExpressionWithPlaceholder.
    auto nestedSchemaMatch = _parse(expCtx,
                                    kNamePlaceholder,
                                    additionalPropertiesElt.embeddedObject(),
                                    allowedFeatures,
                                    ignoreUnknownKeywords);
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
    AllowedFeatureSet allowedFeatures,
    bool ignoreUnknownKeywords,
    bool requiredMissingID) {
    // Collect the set of properties named by the 'properties' keyword.
    StringDataSet propertyNames;
    if (propertiesElt) {
        std::vector<StringData> propertyNamesVec;
        for (auto&& elem : propertiesElt.embeddedObject()) {
            propertyNamesVec.push_back(elem.fieldNameStringData());
        }
        propertyNames.insert(propertyNamesVec.begin(), propertyNamesVec.end());
    }

    auto patternProperties = parsePatternProperties(
        expCtx, patternPropertiesElt, allowedFeatures, ignoreUnknownKeywords);
    if (!patternProperties.isOK()) {
        return patternProperties.getStatus();
    }

    auto patternPropertiesVec = std::move(patternProperties.getValue());

    // If one of the patterns in pattern properties matches '_id', no need to warn about a schema
    // that can't match documents.
    if (requiredMissingID) {
        for (const auto& pattern : patternPropertiesVec) {
            if (pattern.first.regex->matchView("_id", pcre::ANCHORED | pcre::ENDANCHORED)) {
                requiredMissingID = false;
                break;
            }
        }
    }

    auto otherwiseExpr = parseAdditionalProperties(
        expCtx, additionalPropertiesElt, allowedFeatures, ignoreUnknownKeywords, requiredMissingID);
    if (!otherwiseExpr.isOK()) {
        return otherwiseExpr.getStatus();
    }
    auto otherwiseWithPlaceholder = std::make_unique<ExpressionWithPlaceholder>(
        kNamePlaceholder.toString(), std::move(otherwiseExpr.getValue()));

    clonable_ptr<ErrorAnnotation> annotation;
    // In the case of no 'additionalProperties' keyword, but a 'patternProperties' keyword is
    // present, we still want '$_internalSchemaAllowedProperties' to generate an error, so we
    // provide an annotation with empty information.
    if (additionalPropertiesElt.eoo()) {
        annotation = doc_validation_error::createAnnotation(expCtx, "", BSONObj());
    } else {
        annotation =
            doc_validation_error::createAnnotation(expCtx, "", additionalPropertiesElt.wrap());
    }
    auto allowedPropertiesExpr = std::make_unique<InternalSchemaAllowedPropertiesMatchExpression>(
        std::move(propertyNames),
        kNamePlaceholder,
        std::move(patternPropertiesVec),
        std::move(otherwiseWithPlaceholder),
        std::move(annotation));

    // If this is a top-level schema, then we have no path and there is no need for an explicit
    // object match node.
    if (path.empty()) {
        return {std::move(allowedPropertiesExpr)};
    }

    auto objectMatch = std::make_unique<InternalSchemaObjectMatchExpression>(
        path,
        std::move(allowedPropertiesExpr),
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));

    return makeRestriction(expCtx, BSONType::Object, path, std::move(objectMatch), typeExpr);
}

/**
 * Parses 'minProperties' and 'maxProperties' JSON Schema keywords.
 */
template <class T>
StatusWithMatchExpression parseNumProperties(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             StringData path,
                                             BSONElement numProperties,
                                             InternalSchemaTypeExpression* typeExpr) {
    auto parsedNumProps = numProperties.parseIntegerElementToNonNegativeLong();
    if (!parsedNumProps.isOK()) {
        return parsedNumProps.getStatus();
    }

    auto expr = std::make_unique<T>(
        parsedNumProps.getValue(),
        doc_validation_error::createAnnotation(
            expCtx, numProperties.fieldNameStringData().toString(), numProperties.wrap()));

    if (path.empty()) {
        // This is a top-level schema.
        return {std::move(expr)};
    }

    auto objectMatch = std::make_unique<InternalSchemaObjectMatchExpression>(
        path,
        std::move(expr),
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));

    return makeRestriction(expCtx, BSONType::Object, path, std::move(objectMatch), typeExpr);
}

StatusWithMatchExpression makeDependencyExistsClause(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    StringData path,
    StringData dependencyName) {
    // This node is tagged as '_propertyExists' to indicate that it will produce a path instead
    // of a detailed BSONObj error during error generation.
    auto existsExpr = std::make_unique<ExistsMatchExpression>(
        dependencyName,
        doc_validation_error::createAnnotation(expCtx, "_propertyExists", BSONObj()));
    if (path.empty()) {
        return {std::move(existsExpr)};
    }

    auto objectMatch = std::make_unique<InternalSchemaObjectMatchExpression>(
        path,
        std::move(existsExpr),
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));

    return {std::move(objectMatch)};
}

StatusWithMatchExpression translateSchemaDependency(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    StringData path,
    BSONElement dependency,
    AllowedFeatureSet allowedFeatures,
    bool ignoreUnknownKeywords) {
    invariant(dependency.type() == BSONType::Object);

    auto nestedSchemaMatch =
        _parse(expCtx, path, dependency.embeddedObject(), allowedFeatures, ignoreUnknownKeywords);
    if (!nestedSchemaMatch.isOK()) {
        return nestedSchemaMatch.getStatus();
    }

    auto ifClause = makeDependencyExistsClause(expCtx, path, dependency.fieldNameStringData());
    if (!ifClause.isOK()) {
        return ifClause.getStatus();
    }

    // The 'if' should never directly contribute to the error being generated.
    doc_validation_error::annotateTreeToIgnoreForErrorDetails(expCtx, ifClause.getValue().get());

    std::array<std::unique_ptr<MatchExpression>, 3> expressions = {
        std::move(ifClause.getValue()),
        std::move(nestedSchemaMatch.getValue()),
        std::make_unique<AlwaysTrueMatchExpression>(
            doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore))};

    auto condExpr = std::make_unique<InternalSchemaCondMatchExpression>(
        std::move(expressions),
        doc_validation_error::createAnnotation(expCtx, "_schemaDependency", dependency.wrap()));
    return {std::move(condExpr)};
}

StatusWithMatchExpression translatePropertyDependency(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    StringData path,
    BSONElement dependency) {
    invariant(dependency.type() == BSONType::Array);

    if (dependency.embeddedObject().isEmpty()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "property '" << dependency.fieldNameStringData()
                              << "' in $jsonSchema keyword '"
                              << JSONSchemaParser::kSchemaDependenciesKeyword
                              << "' cannot be an empty array"};
    }

    // This node is tagged as '_internalPropertyList' to denote that this node will produce an
    // array of properties during error generation.
    auto propertyDependencyExpr = std::make_unique<AndMatchExpression>(
        doc_validation_error::createAnnotation(expCtx, "_propertiesExistList", dependency.wrap()));
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
            makeDependencyExistsClause(expCtx, path, propertyDependency.valueStringData());
        if (!propertyExistsExpr.isOK()) {
            return propertyExistsExpr.getStatus();
        }

        propertyDependencyExpr->add(std::move(propertyExistsExpr.getValue()));
    }

    auto ifClause = makeDependencyExistsClause(expCtx, path, dependency.fieldNameStringData());
    if (!ifClause.isOK()) {
        return ifClause.getStatus();
    }
    // The 'if' should never directly contribute to the error being generated.
    doc_validation_error::annotateTreeToIgnoreForErrorDetails(expCtx, ifClause.getValue().get());

    std::array<std::unique_ptr<MatchExpression>, 3> expressions = {
        {std::move(ifClause.getValue()),
         std::move(propertyDependencyExpr),
         std::make_unique<AlwaysTrueMatchExpression>(
             doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore))}};

    auto condExpr = std::make_unique<InternalSchemaCondMatchExpression>(
        std::move(expressions),
        doc_validation_error::createAnnotation(expCtx, "_propertyDependency", dependency.wrap()));
    return {std::move(condExpr)};
}

StatusWithMatchExpression parseDependencies(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            StringData path,
                                            BSONElement dependencies,
                                            AllowedFeatureSet allowedFeatures,
                                            bool ignoreUnknownKeywords) {
    if (dependencies.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '"
                              << JSONSchemaParser::kSchemaDependenciesKeyword
                              << "' must be an object"};
    }

    auto andExpr = std::make_unique<AndMatchExpression>(doc_validation_error::createAnnotation(
        expCtx, dependencies.fieldNameStringData().toString(), BSONObj(), dependencies.Obj()));
    for (auto&& dependency : dependencies.embeddedObject()) {
        if (dependency.type() != BSONType::Object && dependency.type() != BSONType::Array) {
            // Allow JSON Schema annotations under "dependency" keyword.
            const auto isSchemaAnnotation =
                dependency.fieldNameStringData() == JSONSchemaParser::kSchemaTitleKeyword ||
                dependency.fieldNameStringData() == JSONSchemaParser::kSchemaDescriptionKeyword;
            if (dependency.type() == BSONType::String && isSchemaAnnotation) {
                continue;
            }

            return {ErrorCodes::TypeMismatch,
                    str::stream() << "property '" << dependency.fieldNameStringData()
                                  << "' in $jsonSchema keyword '"
                                  << JSONSchemaParser::kSchemaDependenciesKeyword
                                  << "' must be either an object or an array"};
        }

        auto dependencyExpr = (dependency.type() == BSONType::Object)
            ? translateSchemaDependency(
                  expCtx, path, dependency, allowedFeatures, ignoreUnknownKeywords)
            : translatePropertyDependency(expCtx, path, dependency);
        if (!dependencyExpr.isOK()) {
            return dependencyExpr.getStatus();
        }

        andExpr->add(std::move(dependencyExpr.getValue()));
    }

    return {std::move(andExpr)};
}

StatusWithMatchExpression parseUniqueItems(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           BSONElement uniqueItemsElt,
                                           StringData path,
                                           InternalSchemaTypeExpression* typeExpr) {
    auto errorAnnotation = doc_validation_error::createAnnotation(
        expCtx, uniqueItemsElt.fieldNameStringData().toString(), uniqueItemsElt.wrap());
    if (!uniqueItemsElt.isBoolean()) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '"
                              << JSONSchemaParser::kSchemaUniqueItemsKeyword
                              << "' must be a boolean"};
    } else if (path.empty()) {
        return {std::make_unique<AlwaysTrueMatchExpression>(std::move(errorAnnotation))};
    } else if (uniqueItemsElt.boolean()) {
        auto uniqueItemsExpr = std::make_unique<InternalSchemaUniqueItemsMatchExpression>(
            path, std::move(errorAnnotation));
        return makeRestriction(expCtx, BSONType::Array, path, std::move(uniqueItemsExpr), typeExpr);
    }

    return {std::make_unique<AlwaysTrueMatchExpression>(std::move(errorAnnotation))};
}

/**
 * Parses 'itemsElt' into a match expression and adds it to 'andExpr'. On success, returns the index
 * from which the "additionalItems" schema should be enforced, if needed.
 */
StatusWith<boost::optional<long long>> parseItems(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    StringData path,
    BSONElement itemsElt,
    AllowedFeatureSet allowedFeatures,
    bool ignoreUnknownKeywords,
    InternalSchemaTypeExpression* typeExpr,
    AndMatchExpression* andExpr) {
    boost::optional<long long> startIndexForAdditionalItems;
    if (itemsElt.type() == BSONType::Array) {
        // When "items" is an array, generate match expressions for each subschema for each position
        // in the array, which are bundled together in an AndMatchExpression. Annotate the
        // AndMatchExpression with the 'items' operator name, since it logically corresponds to the
        // user visible JSON Schema "items" keyword.
        auto andExprForSubschemas =
            std::make_unique<AndMatchExpression>(doc_validation_error::createAnnotation(
                expCtx, itemsElt.fieldNameStringData().toString(), itemsElt.wrap()));
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
            auto parsedSubschema = _parse(expCtx,
                                          kNamePlaceholder,
                                          subschema.embeddedObject(),
                                          allowedFeatures,
                                          ignoreUnknownKeywords);
            if (!parsedSubschema.isOK()) {
                return parsedSubschema.getStatus();
            }
            auto exprWithPlaceholder = std::make_unique<ExpressionWithPlaceholder>(
                kNamePlaceholder.toString(), std::move(parsedSubschema.getValue()));
            auto matchArrayIndex = std::make_unique<InternalSchemaMatchArrayIndexMatchExpression>(
                path,
                index,
                std::move(exprWithPlaceholder),
                doc_validation_error::createAnnotation(
                    expCtx,
                    "" /* 'andExprForSubschemas' carries the operator name, not this expression */,
                    BSONObj()));
            andExprForSubschemas->add(std::move(matchArrayIndex));
            ++index;
        }
        startIndexForAdditionalItems = index;

        if (path.empty()) {
            andExpr->add(
                std::make_unique<AlwaysTrueMatchExpression>(doc_validation_error::createAnnotation(
                    expCtx, itemsElt.fieldNameStringData().toString(), itemsElt.wrap())));
        } else {
            andExpr->add(makeRestriction(
                expCtx, BSONType::Array, path, std::move(andExprForSubschemas), typeExpr));
        }
    } else if (itemsElt.type() == BSONType::Object) {
        // When "items" is an object, generate a single AllElemMatchFromIndex that applies to every
        // element in the array to match. The parsed expression is intended for an
        // ExpressionWithPlaceholder, so we use the default placeholder as the path.
        auto nestedItemsSchema = _parse(expCtx,
                                        kNamePlaceholder,
                                        itemsElt.embeddedObject(),
                                        allowedFeatures,
                                        ignoreUnknownKeywords);
        if (!nestedItemsSchema.isOK()) {
            return nestedItemsSchema.getStatus();
        }
        auto exprWithPlaceholder = std::make_unique<ExpressionWithPlaceholder>(
            kNamePlaceholder.toString(), std::move(nestedItemsSchema.getValue()));

        auto errorAnnotation = doc_validation_error::createAnnotation(
            expCtx, itemsElt.fieldNameStringData().toString(), itemsElt.wrap());
        if (path.empty()) {
            andExpr->add(std::make_unique<AlwaysTrueMatchExpression>(std::move(errorAnnotation)));
        } else {
            constexpr auto startIndexForItems = 0LL;
            auto allElemMatch =
                std::make_unique<InternalSchemaAllElemMatchFromIndexMatchExpression>(
                    path,
                    startIndexForItems,
                    std::move(exprWithPlaceholder),
                    std::move(errorAnnotation));
            andExpr->add(
                makeRestriction(expCtx, BSONType::Array, path, std::move(allElemMatch), typeExpr));
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
                            AllowedFeatureSet allowedFeatures,
                            bool ignoreUnknownKeywords,
                            InternalSchemaTypeExpression* typeExpr,
                            AndMatchExpression* andExpr) {
    std::unique_ptr<ExpressionWithPlaceholder> otherwiseExpr;
    if (additionalItemsElt.type() == BSONType::Bool) {
        const auto emptyPlaceholder = boost::none;
        // Ignore the expression, since InternalSchemaAllElemMatchFromIndexMatchExpression reports
        // the details in this case.
        auto errorAnnotation =
            doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore);
        if (additionalItemsElt.boolean()) {
            otherwiseExpr = std::make_unique<ExpressionWithPlaceholder>(
                emptyPlaceholder,
                std::make_unique<AlwaysTrueMatchExpression>(std::move(errorAnnotation)));
        } else {
            otherwiseExpr = std::make_unique<ExpressionWithPlaceholder>(
                emptyPlaceholder,
                std::make_unique<AlwaysFalseMatchExpression>(std::move(errorAnnotation)));
        }
    } else if (additionalItemsElt.type() == BSONType::Object) {
        auto parsedOtherwiseExpr = _parse(expCtx,
                                          kNamePlaceholder,
                                          additionalItemsElt.embeddedObject(),
                                          allowedFeatures,
                                          ignoreUnknownKeywords);
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
        auto errorAnnotation = doc_validation_error::createAnnotation(
            expCtx, additionalItemsElt.fieldNameStringData().toString(), additionalItemsElt.wrap());
        if (path.empty()) {
            andExpr->add(std::make_unique<AlwaysTrueMatchExpression>(std::move(errorAnnotation)));
        } else {
            auto allElemMatch =
                std::make_unique<InternalSchemaAllElemMatchFromIndexMatchExpression>(
                    path,
                    *startIndexForAdditionalItems,
                    std::move(otherwiseExpr),
                    std::move(errorAnnotation));
            andExpr->add(
                makeRestriction(expCtx, BSONType::Array, path, std::move(allElemMatch), typeExpr));
        }
    }
    return Status::OK();
}

Status parseItemsAndAdditionalItems(StringMap<BSONElement>& keywordMap,
                                    const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    StringData path,
                                    AllowedFeatureSet allowedFeatures,
                                    bool ignoreUnknownKeywords,
                                    InternalSchemaTypeExpression* typeExpr,
                                    AndMatchExpression* andExpr) {
    boost::optional<long long> startIndexForAdditionalItems;
    if (auto itemsElt = keywordMap[JSONSchemaParser::kSchemaItemsKeyword]) {
        auto index = parseItems(
            expCtx, path, itemsElt, allowedFeatures, ignoreUnknownKeywords, typeExpr, andExpr);
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
                                    allowedFeatures,
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
                                AllowedFeatureSet allowedFeatures,
                                bool ignoreUnknownKeywords) {
    if (auto allOfElt = keywordMap[JSONSchemaParser::kSchemaAllOfKeyword]) {
        auto allOfExpr = parseLogicalKeyword<AndMatchExpression>(
            expCtx, path, allOfElt, allowedFeatures, ignoreUnknownKeywords);
        if (!allOfExpr.isOK()) {
            return allOfExpr.getStatus();
        }
        andExpr->add(std::move(allOfExpr.getValue()));
    }

    if (auto anyOfElt = keywordMap[JSONSchemaParser::kSchemaAnyOfKeyword]) {
        auto anyOfExpr = parseLogicalKeyword<OrMatchExpression>(
            expCtx, path, anyOfElt, allowedFeatures, ignoreUnknownKeywords);
        if (!anyOfExpr.isOK()) {
            return anyOfExpr.getStatus();
        }
        andExpr->add(std::move(anyOfExpr.getValue()));
    }

    if (auto oneOfElt = keywordMap[JSONSchemaParser::kSchemaOneOfKeyword]) {
        auto oneOfExpr = parseLogicalKeyword<InternalSchemaXorMatchExpression>(
            expCtx, path, oneOfElt, allowedFeatures, ignoreUnknownKeywords);
        if (!oneOfExpr.isOK()) {
            return oneOfExpr.getStatus();
        }
        andExpr->add(std::move(oneOfExpr.getValue()));
    }

    if (auto notElt = keywordMap[JSONSchemaParser::kSchemaNotKeyword]) {
        if (notElt.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '" << JSONSchemaParser::kSchemaNotKeyword
                                  << "' must be an object, but found an element of type "
                                  << notElt.type()};
        }

        auto parsedExpr =
            _parse(expCtx, path, notElt.embeddedObject(), allowedFeatures, ignoreUnknownKeywords);
        if (!parsedExpr.isOK()) {
            return parsedExpr.getStatus();
        }

        auto notMatchExpr = std::make_unique<NotMatchExpression>(
            std::move(parsedExpr.getValue()),
            doc_validation_error::createAnnotation(expCtx, "not", BSONObj()));
        andExpr->add(std::move(notMatchExpr));
    }

    if (auto enumElt = keywordMap[JSONSchemaParser::kSchemaEnumKeyword]) {
        auto enumExpr = parseEnum(expCtx, path, enumElt);
        if (!enumExpr.isOK()) {
            return enumExpr.getStatus();
        }
        andExpr->add(std::move(enumExpr.getValue()));
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
                              AllowedFeatureSet allowedFeatures,
                              bool ignoreUnknownKeywords,
                              InternalSchemaTypeExpression* typeExpr,
                              AndMatchExpression* andExpr) {
    if (auto minItemsElt = keywordMap[JSONSchemaParser::kSchemaMinItemsKeyword]) {
        auto minItemsExpr = parseLength<InternalSchemaMinItemsMatchExpression>(
            expCtx, path, minItemsElt, typeExpr, BSONType::Array);
        if (!minItemsExpr.isOK()) {
            return minItemsExpr.getStatus();
        }
        andExpr->add(std::move(minItemsExpr.getValue()));
    }

    if (auto maxItemsElt = keywordMap[JSONSchemaParser::kSchemaMaxItemsKeyword]) {
        auto maxItemsExpr = parseLength<InternalSchemaMaxItemsMatchExpression>(
            expCtx, path, maxItemsElt, typeExpr, BSONType::Array);
        if (!maxItemsExpr.isOK()) {
            return maxItemsExpr.getStatus();
        }
        andExpr->add(std::move(maxItemsExpr.getValue()));
    }

    if (auto uniqueItemsElt = keywordMap[JSONSchemaParser::kSchemaUniqueItemsKeyword]) {
        auto uniqueItemsExpr = parseUniqueItems(expCtx, uniqueItemsElt, path, typeExpr);
        if (!uniqueItemsExpr.isOK()) {
            return uniqueItemsExpr.getStatus();
        }
        andExpr->add(std::move(uniqueItemsExpr.getValue()));
    }

    return parseItemsAndAdditionalItems(
        keywordMap, expCtx, path, allowedFeatures, ignoreUnknownKeywords, typeExpr, andExpr);
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
                               AllowedFeatureSet allowedFeatures,
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
        auto propertiesExpr = parseProperties(expCtx,
                                              path,
                                              propertiesElt,
                                              typeExpr,
                                              requiredProperties,
                                              allowedFeatures,
                                              ignoreUnknownKeywords);
        if (!propertiesExpr.isOK()) {
            return propertiesExpr.getStatus();
        }
        andExpr->add(std::move(propertiesExpr.getValue()));
    }

    {
        auto propertiesElt = keywordMap[JSONSchemaParser::kSchemaPropertiesKeyword];
        auto patternPropertiesElt = keywordMap[JSONSchemaParser::kSchemaPatternPropertiesKeyword];
        auto additionalPropertiesElt =
            keywordMap[JSONSchemaParser::kSchemaAdditionalPropertiesKeyword];

        if (patternPropertiesElt || additionalPropertiesElt) {
            // If a top level 'required' field does not contain '_id' and 'additionalProperties' is
            // false, no documents will be permitted. Calculate whether we need to warn the user
            // later in parsing.
            bool requiredMissingID = expCtx->isParsingCollectionValidator && path.empty() &&
                !requiredProperties.contains("_id");
            auto allowedPropertiesExpr = parseAllowedProperties(expCtx,
                                                                path,
                                                                propertiesElt,
                                                                patternPropertiesElt,
                                                                additionalPropertiesElt,
                                                                typeExpr,
                                                                allowedFeatures,
                                                                ignoreUnknownKeywords,
                                                                requiredMissingID);
            if (!allowedPropertiesExpr.isOK()) {
                return allowedPropertiesExpr.getStatus();
            }
            andExpr->add(std::move(allowedPropertiesExpr.getValue()));
        }
    }

    if (!requiredProperties.empty()) {
        auto requiredExpr = translateRequired(expCtx,
                                              requiredProperties,
                                              keywordMap[JSONSchemaParser::kSchemaRequiredKeyword],
                                              path,
                                              typeExpr);
        if (!requiredExpr.isOK()) {
            return requiredExpr.getStatus();
        }
        andExpr->add(std::move(requiredExpr.getValue()));
    }

    if (auto minPropertiesElt = keywordMap[JSONSchemaParser::kSchemaMinPropertiesKeyword]) {
        auto minPropExpr = parseNumProperties<InternalSchemaMinPropertiesMatchExpression>(
            expCtx, path, minPropertiesElt, typeExpr);
        if (!minPropExpr.isOK()) {
            return minPropExpr.getStatus();
        }
        andExpr->add(std::move(minPropExpr.getValue()));
    }

    if (auto maxPropertiesElt = keywordMap[JSONSchemaParser::kSchemaMaxPropertiesKeyword]) {
        auto maxPropExpr = parseNumProperties<InternalSchemaMaxPropertiesMatchExpression>(
            expCtx, path, maxPropertiesElt, typeExpr);
        if (!maxPropExpr.isOK()) {
            return maxPropExpr.getStatus();
        }
        andExpr->add(std::move(maxPropExpr.getValue()));
    }

    if (auto dependenciesElt = keywordMap[JSONSchemaParser::kSchemaDependenciesKeyword]) {
        auto dependenciesExpr = parseDependencies(
            expCtx, path, dependenciesElt, allowedFeatures, ignoreUnknownKeywords);
        if (!dependenciesExpr.isOK()) {
            return dependenciesExpr.getStatus();
        }
        andExpr->add(std::move(dependenciesExpr.getValue()));
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
Status translateScalarKeywords(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               StringMap<BSONElement>& keywordMap,
                               StringData path,
                               InternalSchemaTypeExpression* typeExpr,
                               AndMatchExpression* andExpr) {
    // String keywords.
    if (auto patternElt = keywordMap[JSONSchemaParser::kSchemaPatternKeyword]) {
        auto patternExpr = parsePattern(expCtx, path, patternElt, typeExpr);
        if (!patternExpr.isOK()) {
            return patternExpr.getStatus();
        }
        andExpr->add(std::move(patternExpr.getValue()));
    }

    if (auto maxLengthElt = keywordMap[JSONSchemaParser::kSchemaMaxLengthKeyword]) {
        auto maxLengthExpr = parseLength<InternalSchemaMaxLengthMatchExpression>(
            expCtx, path, maxLengthElt, typeExpr, BSONType::String);
        if (!maxLengthExpr.isOK()) {
            return maxLengthExpr.getStatus();
        }
        andExpr->add(std::move(maxLengthExpr.getValue()));
    }

    if (auto minLengthElt = keywordMap[JSONSchemaParser::kSchemaMinLengthKeyword]) {
        auto minLengthExpr = parseLength<InternalSchemaMinLengthMatchExpression>(
            expCtx, path, minLengthElt, typeExpr, BSONType::String);
        if (!minLengthExpr.isOK()) {
            return minLengthExpr.getStatus();
        }
        andExpr->add(std::move(minLengthExpr.getValue()));
    }

    // Numeric keywords.
    if (auto multipleOfElt = keywordMap[JSONSchemaParser::kSchemaMultipleOfKeyword]) {
        auto multipleOfExpr = parseMultipleOf(expCtx, path, multipleOfElt, typeExpr);
        if (!multipleOfExpr.isOK()) {
            return multipleOfExpr.getStatus();
        }
        andExpr->add(std::move(multipleOfExpr.getValue()));
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
        auto maxExpr = parseMaximum(expCtx, path, maximumElt, typeExpr, isExclusiveMaximum);
        if (!maxExpr.isOK()) {
            return maxExpr.getStatus();
        }
        andExpr->add(std::move(maxExpr.getValue()));
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
        auto minExpr = parseMinimum(expCtx, path, minimumElt, typeExpr, isExclusiveMinimum);
        if (!minExpr.isOK()) {
            return minExpr.getStatus();
        }
        andExpr->add(std::move(minExpr.getValue()));
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
                                   AllowedFeatureSet allowedFeatures,
                                   AndMatchExpression* andExpr) {
    auto encryptElt = keywordMap[JSONSchemaParser::kSchemaEncryptKeyword];
    auto encryptMetadataElt = keywordMap[JSONSchemaParser::kSchemaEncryptMetadataKeyword];

    if ((allowedFeatures & MatchExpressionParser::AllowedFeatures::kEncryptKeywords) == 0u &&
        (encryptElt || encryptMetadataElt))
        return Status(ErrorCodes::QueryFeatureNotAllowed,
                      "Encryption-related validator keywords are not allowed in this context");

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

            andExpr->add(std::make_unique<InternalSchemaBinDataSubTypeExpression>(
                path,
                BinDataType::Encrypt,
                doc_validation_error::createAnnotation(
                    expCtx, encryptElt.fieldNameStringData().toString(), BSONObj())));

            if (auto typeOptional = infoType) {
                andExpr->add(std::make_unique<InternalSchemaBinDataEncryptedTypeExpression>(
                    path,
                    typeOptional->typeSet(),
                    doc_validation_error::createAnnotation(
                        expCtx, encryptElt.fieldNameStringData().toString(), BSONObj())));
            }
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
                                 AllowedFeatureSet allowedFeatures,
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
        auto parsed = parseType(expCtx,
                                path,
                                JSONSchemaParser::kSchemaTypeKeyword,
                                typeElem,
                                MatcherTypeSet::findJsonSchemaTypeAlias);
        if (!parsed.isOK()) {
            return parsed.getStatus();
        }
        typeExpr = std::move(parsed.getValue());
    } else if (bsonTypeElem) {
        auto parseBsonTypeResult = parseType(expCtx,
                                             path,
                                             JSONSchemaParser::kSchemaBsonTypeKeyword,
                                             bsonTypeElem,
                                             findBSONTypeAlias);
        if (!parseBsonTypeResult.isOK()) {
            return parseBsonTypeResult.getStatus();
        }
        typeExpr = std::move(parseBsonTypeResult.getValue());
    } else if (encryptElem) {
        // The presence of the encrypt keyword implies the restriction that the field must be
        // of type BinData.
        typeExpr = std::make_unique<InternalSchemaTypeExpression>(
            path,
            MatcherTypeSet(BSONType::BinData),
            doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));
    }

    // All schemas are given a tag of '_subschema' to indicate during error generation that
    // 'andExpr' logically corresponds to a subschema. If this is a top level schema corresponding
    // to '$jsonSchema', the caller is responsible for providing this information by overwriting
    // this annotation.
    auto andExpr = std::make_unique<AndMatchExpression>(
        doc_validation_error::createAnnotation(expCtx, "_subschema", BSONObj(), schema));

    auto translationStatus =
        translateScalarKeywords(expCtx, keywordMap, path, typeExpr.get(), andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus = translateArrayKeywords(keywordMap,
                                               expCtx,
                                               path,
                                               allowedFeatures,
                                               ignoreUnknownKeywords,
                                               typeExpr.get(),
                                               andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus =
        translateEncryptionKeywords(keywordMap, expCtx, path, allowedFeatures, andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus = translateObjectKeywords(keywordMap,
                                                expCtx,
                                                path,
                                                typeExpr.get(),
                                                andExpr.get(),
                                                allowedFeatures,
                                                ignoreUnknownKeywords);
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus = translateLogicalKeywords(
        keywordMap, expCtx, path, andExpr.get(), allowedFeatures, ignoreUnknownKeywords);
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    if (path.empty() && typeExpr && !typeExpr->typeSet().hasType(BSONType::Object)) {
        // This is a top-level schema which requires that the type is something other than
        // "object". Since we only know how to store objects, this schema matches nothing.
        return {std::make_unique<AlwaysFalseMatchExpression>(doc_validation_error::createAnnotation(
            expCtx, "$jsonSchema", BSON("$jsonSchema" << schema)))};
    }

    if (!path.empty() && typeExpr) {
        andExpr->add(std::move(typeExpr));
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
    AllowedFeatureSet allowedFeatures,
    bool ignoreUnknownKeywords) {
    LOGV2_DEBUG(20728,
                5,
                "Parsing JSON Schema: {schema}",
                "Parsing JSON Schema",
                "schema"_attr = schema.jsonString(JsonStringFormat::LegacyStrict));
    try {
        auto translation = _parse(expCtx, ""_sd, schema, allowedFeatures, ignoreUnknownKeywords);
        if (shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(5)) &&
            translation.isOK()) {
            LOGV2_DEBUG(20729,
                        5,
                        "Translated schema match expression: {expression}",
                        "Translated schema match expression",
                        "expression"_attr = translation.getValue()->debugString());
        }
        // Tag the top level MatchExpression as '$jsonSchema' if necessary.
        if (translation.isOK()) {
            if (auto topLevelAnnotation = translation.getValue()->getErrorAnnotation()) {
                auto oldAnnotation = topLevelAnnotation->annotation;
                translation.getValue()->setErrorAnnotation(doc_validation_error::createAnnotation(
                    expCtx, "$jsonSchema", oldAnnotation, schema));
            }
        }
        expCtx->sbeCompatible = false;
        return translation;
    } catch (const DBException& ex) {
        return {ex.toStatus()};
    }
}
}  // namespace mongo

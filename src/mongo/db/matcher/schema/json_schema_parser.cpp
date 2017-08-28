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

#include <boost/container/flat_set.hpp>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/string_map.h"

namespace mongo {

namespace {
// Standard JSON Schema keyword constants.
constexpr StringData kSchemaAdditionalItemsKeyword = "additionalItems"_sd;
constexpr StringData kSchemaAllOfKeyword = "allOf"_sd;
constexpr StringData kSchemaAnyOfKeyword = "anyOf"_sd;
constexpr StringData kSchemaDependenciesKeyword = "dependencies"_sd;
constexpr StringData kSchemaExclusiveMaximumKeyword = "exclusiveMaximum"_sd;
constexpr StringData kSchemaExclusiveMinimumKeyword = "exclusiveMinimum"_sd;
constexpr StringData kSchemaItemsKeyword = "items"_sd;
constexpr StringData kSchemaMaxItemsKeyword = "maxItems"_sd;
constexpr StringData kSchemaMaxLengthKeyword = "maxLength"_sd;
constexpr StringData kSchemaMaxPropertiesKeyword = "maxProperties"_sd;
constexpr StringData kSchemaMaximumKeyword = "maximum"_sd;
constexpr StringData kSchemaMinItemsKeyword = "minItems"_sd;
constexpr StringData kSchemaMinLengthKeyword = "minLength"_sd;
constexpr StringData kSchemaMinPropertiesKeyword = "minProperties"_sd;
constexpr StringData kSchemaMinimumKeyword = "minimum"_sd;
constexpr StringData kSchemaMultipleOfKeyword = "multipleOf"_sd;
constexpr StringData kSchemaNotKeyword = "not"_sd;
constexpr StringData kSchemaOneOfKeyword = "oneOf"_sd;
constexpr StringData kSchemaPatternKeyword = "pattern"_sd;
constexpr StringData kSchemaPropertiesKeyword = "properties"_sd;
constexpr StringData kSchemaRequiredKeyword = "required"_sd;
constexpr StringData kSchemaTypeKeyword = "type"_sd;
constexpr StringData kSchemaUniqueItemsKeyword = "uniqueItems"_sd;

// MongoDB-specific (non-standard) JSON Schema keyword constants.
constexpr StringData kSchemaBsonTypeKeyword = "bsonType"_sd;

/**
 * Parses 'schema' to the semantically equivalent match expression. If the schema has an associated
 * path, e.g. if we are parsing the nested schema for property "myProp" in
 *
 *    {properties: {myProp: <nested-schema>}}
 *
 * then this is passed in 'path'. In this example, the value of 'path' is "myProp". If there is no
 * path, e.g. for top-level schemas, then 'path' is empty.
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
    invariantOK(typeExpr->init(path, restrictionType));

    auto notExpr = stdx::make_unique<NotMatchExpression>();
    invariantOK(notExpr->init(typeExpr.release()));

    auto orExpr = stdx::make_unique<OrMatchExpression>();
    orExpr->add(notExpr.release());
    orExpr->add(restrictionExpr.release());

    return std::move(orExpr);
}

StatusWith<std::unique_ptr<InternalSchemaTypeExpression>> parseType(
    StringData path,
    StringData keywordName,
    BSONElement typeElt,
    const StringMap<BSONType>& aliasMap) {
    if (typeElt.type() != BSONType::String && typeElt.type() != BSONType::Array) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << "$jsonSchema keyword '" << keywordName
                                     << "' must be either a string or an array of strings")};
    }

    std::set<StringData> aliases;
    if (typeElt.type() == BSONType::String) {
        aliases.insert(typeElt.valueStringData());
    } else {
        for (auto&& typeArrayEntry : typeElt.embeddedObject()) {
            if (typeArrayEntry.type() != BSONType::String) {
                return {Status(ErrorCodes::TypeMismatch,
                               str::stream() << "$jsonSchema keyword '" << keywordName
                                             << "' array elements must be strings")};
            }

            auto insertionResult = aliases.insert(typeArrayEntry.valueStringData());
            if (!insertionResult.second) {
                return {Status(ErrorCodes::FailedToParse,
                               str::stream() << "$jsonSchema keyword '" << keywordName
                                             << "' has duplicate value: "
                                             << typeArrayEntry.valueStringData())};
            }
        }
    }

    auto typeSet = MatcherTypeSet::fromStringAliases(std::move(aliases), aliasMap);
    if (!typeSet.isOK()) {
        return typeSet.getStatus();
    }

    if (typeSet.getValue().isEmpty()) {
        return {Status(ErrorCodes::FailedToParse,
                       str::stream() << "$jsonSchema keyword '" << keywordName
                                     << "' must name at least one type")};
    }

    auto typeExpr = stdx::make_unique<InternalSchemaTypeExpression>();
    auto initStatus = typeExpr->init(path, std::move(typeSet.getValue()));
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
    auto parsedLength = MatchExpressionParser::parseIntegerElementToNonNegativeLong(length);
    if (!parsedLength.isOK()) {
        return parsedLength.getStatus();
    }

    if (path.empty()) {
        return {stdx::make_unique<AlwaysTrueMatchExpression>()};
    }

    auto expr = stdx::make_unique<T>();
    auto status = expr->init(path, parsedLength.getValue());
    if (!status.isOK()) {
        return status;
    }
    return makeRestriction(restrictionType, path, std::move(expr), typeExpr);
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
    return makeRestriction(BSONType::String, path, std::move(expr), typeExpr);
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

    MatcherTypeSet restrictionType;
    restrictionType.allNumbers = true;
    return makeRestriction(restrictionType, path, std::move(expr), typeExpr);
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
 * Given a BSON element corresponding to the $jsonSchema "required" keyword, returns the set of
 * required property names. If the contents of the "required" keyword are invalid, returns a non-OK
 * status.
 */
StatusWith<boost::container::flat_set<StringData>> parseRequired(BSONElement requiredElt) {
    if (requiredElt.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '" << kSchemaRequiredKeyword
                              << "' must be an array, but found an element of type "
                              << requiredElt.type()};
    }

    std::vector<StringData> propertyVec;
    for (auto&& propertyName : requiredElt.embeddedObject()) {
        if (propertyName.type() != BSONType::String) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "$jsonSchema keyword '" << kSchemaRequiredKeyword
                                  << "' must be an array of strings, but found an element of type: "
                                  << propertyName.type()};
        }

        propertyVec.push_back(propertyName.valueStringData());
    }

    if (propertyVec.empty()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "$jsonSchema keyword '" << kSchemaRequiredKeyword
                              << "' cannot be an empty array"};
    }

    boost::container::flat_set<StringData> requiredProperties{propertyVec.begin(),
                                                              propertyVec.end()};
    if (requiredProperties.size() != propertyVec.size()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "$jsonSchema keyword '" << kSchemaRequiredKeyword
                              << "' array cannot contain duplicate values"};
    }
    return requiredProperties;
}

/**
 * Given the already-parsed set of required properties, returns a MatchExpression which ensures that
 * those properties exist. Returns a parsing error if the translation fails.
 */
StatusWithMatchExpression translateRequired(
    const boost::container::flat_set<StringData>& requiredProperties,
    StringData path,
    InternalSchemaTypeExpression* typeExpr) {
    auto andExpr = stdx::make_unique<AndMatchExpression>();

    for (auto&& propertyName : requiredProperties) {
        auto existsExpr = stdx::make_unique<ExistsMatchExpression>();
        invariantOK(existsExpr->init(propertyName));

        if (path.empty()) {
            andExpr->add(existsExpr.release());
        } else {
            auto objectMatch = stdx::make_unique<InternalSchemaObjectMatchExpression>();
            auto objectMatchStatus = objectMatch->init(std::move(existsExpr), path);
            if (!objectMatchStatus.isOK()) {
                return objectMatchStatus;
            }

            andExpr->add(objectMatch.release());
        }
    }

    // If this is a top-level schema, then we know that we are matching against objects, and there
    // is no need to worry about ensuring that non-objects match.
    if (path.empty()) {
        return {std::move(andExpr)};
    }

    return makeRestriction(BSONType::Object, path, std::move(andExpr), typeExpr);
}

StatusWithMatchExpression parseProperties(
    StringData path,
    BSONElement propertiesElt,
    InternalSchemaTypeExpression* typeExpr,
    const boost::container::flat_set<StringData>& requiredProperties) {
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

        if (requiredProperties.find(property.fieldNameStringData()) != requiredProperties.end()) {
            // The field name for which we created the nested schema is a required property. This
            // property must exist and therefore must match 'nestedSchemaMatch'.
            andExpr->add(nestedSchemaMatch.getValue().release());
        } else {
            // This property either must not exist or must match the nested schema. Therefore, we
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

    return makeRestriction(BSONType::Object, path, std::move(objectMatch), typeExpr);
}

/**
 * Parses 'minProperties' and 'maxProperties' JSON Schema keywords.
 */
template <class T>
StatusWithMatchExpression parseNumProperties(StringData path,
                                             BSONElement numProperties,
                                             InternalSchemaTypeExpression* typeExpr) {
    auto parsedNumProps =
        MatchExpressionParser::parseIntegerElementToNonNegativeLong(numProperties);
    if (!parsedNumProps.isOK()) {
        return parsedNumProps.getStatus();
    }

    auto expr = stdx::make_unique<T>();
    auto status = expr->init(parsedNumProps.getValue());
    if (!status.isOK()) {
        return status;
    }

    if (path.empty()) {
        // This is a top-level schema.
        return {std::move(expr)};
    }

    auto objectMatch = stdx::make_unique<InternalSchemaObjectMatchExpression>();
    auto objectMatchStatus = objectMatch->init(std::move(expr), path);
    if (!objectMatchStatus.isOK()) {
        return objectMatchStatus;
    }

    return makeRestriction(BSONType::Object, path, std::move(objectMatch), typeExpr);
}

StatusWithMatchExpression makeDependencyExistsClause(StringData path, StringData dependencyName) {
    auto existsExpr = stdx::make_unique<ExistsMatchExpression>();
    invariantOK(existsExpr->init(dependencyName));

    if (path.empty()) {
        return {std::move(existsExpr)};
    }

    auto objectMatch = stdx::make_unique<InternalSchemaObjectMatchExpression>();
    auto status = objectMatch->init(std::move(existsExpr), path);
    if (!status.isOK()) {
        return status;
    }

    return {std::move(objectMatch)};
}

StatusWithMatchExpression translateSchemaDependency(StringData path, BSONElement dependency) {
    invariant(dependency.type() == BSONType::Object);

    auto nestedSchemaMatch = _parse(path, dependency.embeddedObject());
    if (!nestedSchemaMatch.isOK()) {
        return nestedSchemaMatch.getStatus();
    }

    auto ifClause = makeDependencyExistsClause(path, dependency.fieldNameStringData());
    if (!ifClause.isOK()) {
        return ifClause.getStatus();
    }

    auto condExpr = stdx::make_unique<InternalSchemaCondMatchExpression>();
    condExpr->init({std::move(ifClause.getValue()),
                    std::move(nestedSchemaMatch.getValue()),
                    stdx::make_unique<AlwaysTrueMatchExpression>()});
    return {std::move(condExpr)};
}

StatusWithMatchExpression translatePropertyDependency(StringData path, BSONElement dependency) {
    invariant(dependency.type() == BSONType::Array);

    if (dependency.embeddedObject().isEmpty()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "property '" << dependency.fieldNameStringData()
                              << "' in $jsonSchema keyword '"
                              << kSchemaDependenciesKeyword
                              << "' cannot be an empty array"};
    }

    auto propertyDependencyExpr = stdx::make_unique<AndMatchExpression>();
    std::set<StringData> propertyDependencyNames;
    for (auto&& propertyDependency : dependency.embeddedObject()) {
        if (propertyDependency.type() != BSONType::String) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "array '" << dependency.fieldNameStringData()
                                  << "' in $jsonSchema keyword '"
                                  << kSchemaDependenciesKeyword
                                  << "' can only contain strings, but found element of type: "
                                  << typeName(propertyDependency.type())};
        }

        auto insertionResult = propertyDependencyNames.insert(propertyDependency.valueStringData());
        if (!insertionResult.second) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "array '" << dependency.fieldNameStringData()
                                  << "' in $jsonSchema keyword '"
                                  << kSchemaDependenciesKeyword
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

    auto condExpr = stdx::make_unique<InternalSchemaCondMatchExpression>();
    condExpr->init({std::move(ifClause.getValue()),
                    std::move(propertyDependencyExpr),
                    stdx::make_unique<AlwaysTrueMatchExpression>()});
    return {std::move(condExpr)};
}

StatusWithMatchExpression parseDependencies(StringData path, BSONElement dependencies) {
    if (dependencies.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "$jsonSchema keyword '" << kSchemaDependenciesKeyword
                              << "' must be an object"};
    }

    if (dependencies.embeddedObject().isEmpty()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "$jsonSchema keyword '" << kSchemaDependenciesKeyword
                              << "' must be a non-empty object"};
    }

    auto andExpr = stdx::make_unique<AndMatchExpression>();
    for (auto&& dependency : dependencies.embeddedObject()) {
        if (dependency.type() != BSONType::Object && dependency.type() != BSONType::Array) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "property '" << dependency.fieldNameStringData()
                                  << "' in $jsonSchema keyword '"
                                  << kSchemaDependenciesKeyword
                                  << "' must be either an object or an array"};
        }

        auto dependencyExpr = (dependency.type() == BSONType::Object)
            ? translateSchemaDependency(path, dependency)
            : translatePropertyDependency(path, dependency);
        if (!dependencyExpr.isOK()) {
            return dependencyExpr.getStatus();
        }

        andExpr->add(dependencyExpr.getValue().release());
    }

    return {std::move(andExpr)};
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
Status translateLogicalKeywords(StringMap<BSONElement>* keywordMap,
                                StringData path,
                                AndMatchExpression* andExpr) {
    if (auto allOfElt = keywordMap->get(kSchemaAllOfKeyword)) {
        auto allOfExpr = parseLogicalKeyword<AndMatchExpression>(path, allOfElt);
        if (!allOfExpr.isOK()) {
            return allOfExpr.getStatus();
        }
        andExpr->add(allOfExpr.getValue().release());
    }

    if (auto anyOfElt = keywordMap->get(kSchemaAnyOfKeyword)) {
        auto anyOfExpr = parseLogicalKeyword<OrMatchExpression>(path, anyOfElt);
        if (!anyOfExpr.isOK()) {
            return anyOfExpr.getStatus();
        }
        andExpr->add(anyOfExpr.getValue().release());
    }

    if (auto oneOfElt = keywordMap->get(kSchemaOneOfKeyword)) {
        auto oneOfExpr = parseLogicalKeyword<InternalSchemaXorMatchExpression>(path, oneOfElt);
        if (!oneOfExpr.isOK()) {
            return oneOfExpr.getStatus();
        }
        andExpr->add(oneOfExpr.getValue().release());
    }

    if (auto notElt = keywordMap->get(kSchemaNotKeyword)) {
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
Status translateArrayKeywords(StringMap<BSONElement>* keywordMap,
                              StringData path,
                              InternalSchemaTypeExpression* typeExpr,
                              AndMatchExpression* andExpr) {
    if (auto minItemsElt = keywordMap->get(kSchemaMinItemsKeyword)) {
        auto minItemsExpr = parseLength<InternalSchemaMinItemsMatchExpression>(
            path, minItemsElt, typeExpr, BSONType::Array);
        if (!minItemsExpr.isOK()) {
            return minItemsExpr.getStatus();
        }
        andExpr->add(minItemsExpr.getValue().release());
    }

    if (auto maxItemsElt = keywordMap->get(kSchemaMaxItemsKeyword)) {
        auto maxItemsExpr = parseLength<InternalSchemaMaxItemsMatchExpression>(
            path, maxItemsElt, typeExpr, BSONType::Array);
        if (!maxItemsExpr.isOK()) {
            return maxItemsExpr.getStatus();
        }
        andExpr->add(maxItemsExpr.getValue().release());
    }

    return Status::OK();
}

/**
 * Parses JSON Schema keywords related to objects in 'keywordMap' and adds them to 'andExpr'.
 * Returns a non-OK status if an error occurs during parsing.
 *
 * This function parses the following keywords:
 *  - dependencies
 *  - maxProperties
 *  - minProperties
 *  - properties
 *  - required
 */
Status translateObjectKeywords(StringMap<BSONElement>* keywordMap,
                               StringData path,
                               InternalSchemaTypeExpression* typeExpr,
                               AndMatchExpression* andExpr) {
    boost::container::flat_set<StringData> requiredProperties;
    if (auto requiredElt = keywordMap->get(kSchemaRequiredKeyword)) {
        auto requiredStatus = parseRequired(requiredElt);
        if (!requiredStatus.isOK()) {
            return requiredStatus.getStatus();
        }
        requiredProperties = std::move(requiredStatus.getValue());
    }

    if (auto propertiesElt = keywordMap->get(kSchemaPropertiesKeyword)) {
        auto propertiesExpr = parseProperties(path, propertiesElt, typeExpr, requiredProperties);
        if (!propertiesExpr.isOK()) {
            return propertiesExpr.getStatus();
        }
        andExpr->add(propertiesExpr.getValue().release());
    }

    if (!requiredProperties.empty()) {
        auto requiredExpr = translateRequired(requiredProperties, path, typeExpr);
        if (!requiredExpr.isOK()) {
            return requiredExpr.getStatus();
        }
        andExpr->add(requiredExpr.getValue().release());
    }

    if (auto minPropertiesElt = keywordMap->get(kSchemaMinPropertiesKeyword)) {
        auto minPropExpr = parseNumProperties<InternalSchemaMinPropertiesMatchExpression>(
            path, minPropertiesElt, typeExpr);
        if (!minPropExpr.isOK()) {
            return minPropExpr.getStatus();
        }
        andExpr->add(minPropExpr.getValue().release());
    }

    if (auto maxPropertiesElt = keywordMap->get(kSchemaMaxPropertiesKeyword)) {
        auto maxPropExpr = parseNumProperties<InternalSchemaMaxPropertiesMatchExpression>(
            path, maxPropertiesElt, typeExpr);
        if (!maxPropExpr.isOK()) {
            return maxPropExpr.getStatus();
        }
        andExpr->add(maxPropExpr.getValue().release());
    }

    if (auto dependenciesElt = keywordMap->get(kSchemaDependenciesKeyword)) {
        auto dependenciesExpr = parseDependencies(path, dependenciesElt);
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
 */
Status translateScalarKeywords(StringMap<BSONElement>* keywordMap,
                               StringData path,
                               InternalSchemaTypeExpression* typeExpr,
                               AndMatchExpression* andExpr) {
    // String keywords.
    if (auto patternElt = keywordMap->get(kSchemaPatternKeyword)) {
        auto patternExpr = parsePattern(path, patternElt, typeExpr);
        if (!patternExpr.isOK()) {
            return patternExpr.getStatus();
        }
        andExpr->add(patternExpr.getValue().release());
    }

    if (auto maxLengthElt = keywordMap->get(kSchemaMaxLengthKeyword)) {
        auto maxLengthExpr = parseLength<InternalSchemaMaxLengthMatchExpression>(
            path, maxLengthElt, typeExpr, BSONType::String);
        if (!maxLengthExpr.isOK()) {
            return maxLengthExpr.getStatus();
        }
        andExpr->add(maxLengthExpr.getValue().release());
    }

    if (auto minLengthElt = keywordMap->get(kSchemaMinLengthKeyword)) {
        auto minLengthExpr = parseLength<InternalSchemaMinLengthMatchExpression>(
            path, minLengthElt, typeExpr, BSONType::String);
        if (!minLengthExpr.isOK()) {
            return minLengthExpr.getStatus();
        }
        andExpr->add(minLengthExpr.getValue().release());
    }

    // Numeric keywords.
    if (auto multipleOfElt = keywordMap->get(kSchemaMultipleOfKeyword)) {
        auto multipleOfExpr = parseMultipleOf(path, multipleOfElt, typeExpr);
        if (!multipleOfExpr.isOK()) {
            return multipleOfExpr.getStatus();
        }
        andExpr->add(multipleOfExpr.getValue().release());
    }

    if (auto maximumElt = keywordMap->get(kSchemaMaximumKeyword)) {
        bool isExclusiveMaximum = false;
        if (auto exclusiveMaximumElt = keywordMap->get(kSchemaExclusiveMaximumKeyword)) {
            if (!exclusiveMaximumElt.isBoolean()) {
                return {Status(ErrorCodes::TypeMismatch,
                               str::stream() << "$jsonSchema keyword '"
                                             << kSchemaExclusiveMaximumKeyword
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
    } else if (keywordMap->get(kSchemaExclusiveMaximumKeyword)) {
        // If "exclusiveMaximum" is present, "maximum" must also be present.
        return {ErrorCodes::FailedToParse,
                str::stream() << "$jsonSchema keyword '" << kSchemaMaximumKeyword
                              << "' must be a present if "
                              << kSchemaExclusiveMaximumKeyword
                              << " is present"};
    }

    if (auto minimumElt = keywordMap->get(kSchemaMinimumKeyword)) {
        bool isExclusiveMinimum = false;
        if (auto exclusiveMinimumElt = keywordMap->get(kSchemaExclusiveMinimumKeyword)) {
            if (!exclusiveMinimumElt.isBoolean()) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "$jsonSchema keyword '" << kSchemaExclusiveMinimumKeyword
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
    } else if (keywordMap->get(kSchemaExclusiveMinimumKeyword)) {
        // If "exclusiveMinimum" is present, "minimum" must also be present.
        return {ErrorCodes::FailedToParse,
                str::stream() << "$jsonSchema keyword '" << kSchemaMinimumKeyword
                              << "' must be a present if "
                              << kSchemaExclusiveMinimumKeyword
                              << " is present"};
    }

    return Status::OK();
}

StatusWithMatchExpression _parse(StringData path, BSONObj schema) {
    // Map from JSON Schema keyword to the corresponding element from 'schema', or to an empty
    // BSONElement if the JSON Schema keyword is not specified.
    StringMap<BSONElement> keywordMap{
        {kSchemaAllOfKeyword, {}},
        {kSchemaAnyOfKeyword, {}},
        {kSchemaBsonTypeKeyword, {}},
        {kSchemaDependenciesKeyword, {}},
        {kSchemaExclusiveMaximumKeyword, {}},
        {kSchemaExclusiveMinimumKeyword, {}},
        {kSchemaMaxItemsKeyword, {}},
        {kSchemaMaxLengthKeyword, {}},
        {kSchemaMaxPropertiesKeyword, {}},
        {kSchemaMaximumKeyword, {}},
        {kSchemaMinItemsKeyword, {}},
        {kSchemaMinLengthKeyword, {}},
        {kSchemaMinPropertiesKeyword, {}},
        {kSchemaMinimumKeyword, {}},
        {kSchemaMultipleOfKeyword, {}},
        {kSchemaNotKeyword, {}},
        {kSchemaOneOfKeyword, {}},
        {kSchemaPatternKeyword, {}},
        {kSchemaPropertiesKeyword, {}},
        {kSchemaRequiredKeyword, {}},
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

    auto typeElem = keywordMap[kSchemaTypeKeyword];
    auto bsonTypeElem = keywordMap[kSchemaBsonTypeKeyword];
    if (typeElem && bsonTypeElem) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Cannot specify both $jsonSchema keywords '"
                                    << kSchemaTypeKeyword
                                    << "' and '"
                                    << kSchemaBsonTypeKeyword
                                    << "'");
    }

    std::unique_ptr<InternalSchemaTypeExpression> typeExpr;
    if (typeElem) {
        auto parseTypeResult =
            parseType(path, kSchemaTypeKeyword, typeElem, MatcherTypeSet::kJsonSchemaTypeAliasMap);
        if (!parseTypeResult.isOK()) {
            return parseTypeResult.getStatus();
        }
        typeExpr = std::move(parseTypeResult.getValue());
    } else if (bsonTypeElem) {
        auto parseBsonTypeResult =
            parseType(path, kSchemaBsonTypeKeyword, bsonTypeElem, MatcherTypeSet::kTypeAliasMap);
        if (!parseBsonTypeResult.isOK()) {
            return parseBsonTypeResult.getStatus();
        }
        typeExpr = std::move(parseBsonTypeResult.getValue());
    }

    auto andExpr = stdx::make_unique<AndMatchExpression>();

    auto translationStatus =
        translateScalarKeywords(&keywordMap, path, typeExpr.get(), andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus = translateArrayKeywords(&keywordMap, path, typeExpr.get(), andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus = translateObjectKeywords(&keywordMap, path, typeExpr.get(), andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    translationStatus = translateLogicalKeywords(&keywordMap, path, andExpr.get());
    if (!translationStatus.isOK()) {
        return translationStatus;
    }

    if (path.empty() && typeExpr && !typeExpr->typeSet().hasType(BSONType::Object)) {
        // This is a top-level schema which requires that the type is something other than
        // "object". Since we only know how to store objects, this schema matches nothing.
        return {stdx::make_unique<AlwaysFalseMatchExpression>()};
    }

    if (!path.empty() && typeExpr) {
        andExpr->add(typeExpr.release());
    }
    return {std::move(andExpr)};
}
}  // namespace

constexpr StringData JSONSchemaParser::kSchemaTypeArray;
constexpr StringData JSONSchemaParser::kSchemaTypeBoolean;
constexpr StringData JSONSchemaParser::kSchemaTypeNull;
constexpr StringData JSONSchemaParser::kSchemaTypeObject;
constexpr StringData JSONSchemaParser::kSchemaTypeString;

StatusWithMatchExpression JSONSchemaParser::parse(BSONObj schema) {
    return _parse(StringData{}, schema);
}

}  // namespace mongo

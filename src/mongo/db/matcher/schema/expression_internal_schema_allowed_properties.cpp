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

#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <type_traits>

#include <absl/container/node_hash_map.h>

namespace mongo {
constexpr StringData InternalSchemaAllowedPropertiesMatchExpression::kName;

InternalSchemaAllowedPropertiesMatchExpression::InternalSchemaAllowedPropertiesMatchExpression(
    StringDataSet properties,
    StringData namePlaceholder,
    std::vector<PatternSchema> patternProperties,
    std::unique_ptr<ExpressionWithPlaceholder> otherwise,
    clonable_ptr<ErrorAnnotation> annotation)
    : MatchExpression(MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES, std::move(annotation)),
      _properties(std::move(properties)),
      _namePlaceholder(namePlaceholder),
      _patternProperties(std::move(patternProperties)),
      _otherwise(std::move(otherwise)) {

    for (auto&& constraint : _patternProperties) {
        const auto& re = constraint.first.regex;
        uassert(ErrorCodes::BadValue,
                str::stream() << "Invalid regular expression: " << errorMessage(re->error()),
                *re);
    }
}

void InternalSchemaAllowedPropertiesMatchExpression::debugString(StringBuilder& debug,
                                                                 int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    BSONObjBuilder builder;
    serialize(&builder, {});
    debug << builder.obj().toString();
    _debugStringAttachTagInfo(&debug);
}

bool InternalSchemaAllowedPropertiesMatchExpression::equivalent(const MatchExpression* expr) const {
    if (matchType() != expr->matchType()) {
        return false;
    }

    const auto* other = static_cast<const InternalSchemaAllowedPropertiesMatchExpression*>(expr);
    return _properties == other->_properties && _namePlaceholder == other->_namePlaceholder &&
        _otherwise->equivalent(other->_otherwise.get()) &&
        std::is_permutation(_patternProperties.begin(),
                            _patternProperties.end(),
                            other->_patternProperties.begin(),
                            other->_patternProperties.end(),
                            [](const auto& expr1, const auto& expr2) {
                                return expr1.first.rawRegex == expr2.first.rawRegex &&
                                    expr1.second->equivalent(expr2.second.get());
                            });
}

void InternalSchemaAllowedPropertiesMatchExpression::serialize(BSONObjBuilder* builder,
                                                               const SerializationOptions& opts,
                                                               bool includePath) const {
    BSONObjBuilder expressionBuilder(
        builder->subobjStart(InternalSchemaAllowedPropertiesMatchExpression::kName));

    std::vector<StringData> sortedProperties(_properties.begin(), _properties.end());
    std::sort(sortedProperties.begin(), sortedProperties.end());
    opts.appendLiteral(&expressionBuilder, "properties", sortedProperties);
    // This will be serialized to "i", which is the parser chosen namePlaceholder. Using this
    // unmodified will have a similar effect to serializing to "?", however it preserves round trip
    // parsing.
    expressionBuilder.append("namePlaceholder", _namePlaceholder);

    BSONArrayBuilder patternPropertiesBuilder(expressionBuilder.subarrayStart("patternProperties"));
    for (auto&& [pattern, expression] : _patternProperties) {
        patternPropertiesBuilder << BSON(
            "regex" << opts.serializeLiteral(BSONRegEx(pattern.rawRegex)) << "expression"
                    << expression->getFilter()->serialize(opts, includePath));
    }
    patternPropertiesBuilder.doneFast();

    BSONObjBuilder otherwiseBuilder(expressionBuilder.subobjStart("otherwise"));
    _otherwise->getFilter()->serialize(&otherwiseBuilder, opts, includePath);
    otherwiseBuilder.doneFast();
    expressionBuilder.doneFast();
}

std::unique_ptr<MatchExpression> InternalSchemaAllowedPropertiesMatchExpression::clone() const {
    std::vector<PatternSchema> clonedPatternProperties;
    clonedPatternProperties.reserve(_patternProperties.size());
    for (auto&& constraint : _patternProperties) {
        clonedPatternProperties.emplace_back(Pattern(constraint.first.rawRegex),
                                             constraint.second->clone());
    }

    auto clone = std::make_unique<InternalSchemaAllowedPropertiesMatchExpression>(
        _properties,
        _namePlaceholder,
        std::move(clonedPatternProperties),
        _otherwise->clone(),
        _errorAnnotation);
    return {std::move(clone)};
}

}  // namespace mongo

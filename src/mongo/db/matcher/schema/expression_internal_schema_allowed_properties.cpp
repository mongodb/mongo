// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <string_view>
#include <type_traits>

#include <absl/container/node_hash_map.h>

namespace mongo {
constexpr std::string_view InternalSchemaAllowedPropertiesMatchExpression::kName;

InternalSchemaAllowedPropertiesMatchExpression::InternalSchemaAllowedPropertiesMatchExpression(
    StringDataSet properties,
    std::string_view namePlaceholder,
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

void InternalSchemaAllowedPropertiesMatchExpression::serialize(
    BSONObjBuilder* builder,
    const query_shape::SerializationOptions& opts,
    bool includePath) const {
    BSONObjBuilder expressionBuilder(
        builder->subobjStart(InternalSchemaAllowedPropertiesMatchExpression::kName));

    std::vector<std::string_view> sortedProperties(_properties.begin(), _properties.end());
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

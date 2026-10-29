// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/value.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
constexpr std::string_view InternalSchemaMatchArrayIndexMatchExpression::kName;

InternalSchemaMatchArrayIndexMatchExpression::InternalSchemaMatchArrayIndexMatchExpression(
    boost::optional<std::string_view> path,
    long long index,
    std::unique_ptr<ExpressionWithPlaceholder> expression,
    clonable_ptr<ErrorAnnotation> annotation)
    : ArrayMatchingMatchExpression(
          MatchExpression::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX, path, std::move(annotation)),
      _index(index),
      _expression(std::move(expression)) {
    invariant(static_cast<bool>(_expression));
}

void InternalSchemaMatchArrayIndexMatchExpression::debugString(StringBuilder& debug,
                                                               int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    BSONObjBuilder builder;
    serialize(&builder, {});
    debug << builder.obj().toString();
    _debugStringAttachTagInfo(&debug);
}

bool InternalSchemaMatchArrayIndexMatchExpression::equivalent(const MatchExpression* expr) const {
    if (matchType() != expr->matchType()) {
        return false;
    }

    const auto* other = static_cast<const InternalSchemaMatchArrayIndexMatchExpression*>(expr);
    return path() == other->path() && _index == other->_index &&
        _expression->equivalent(other->_expression.get());
}

void InternalSchemaMatchArrayIndexMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    bob->append(
        kName,
        BSON(
            "index" << opts.serializeLiteral(_index) << "namePlaceholder"
                    << opts.serializeFieldPathFromString(_expression->getPlaceholder().value_or(""))
                    << "expression" << _expression->getFilter()->serialize(opts, includePath)));
}

std::unique_ptr<MatchExpression> InternalSchemaMatchArrayIndexMatchExpression::clone() const {
    auto clone = std::make_unique<InternalSchemaMatchArrayIndexMatchExpression>(
        path(), _index, _expression->clone(), _errorAnnotation);
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return clone;
}

}  // namespace mongo

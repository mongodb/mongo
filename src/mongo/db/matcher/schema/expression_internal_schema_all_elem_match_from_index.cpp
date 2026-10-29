// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

constexpr std::string_view InternalSchemaAllElemMatchFromIndexMatchExpression::kName;

InternalSchemaAllElemMatchFromIndexMatchExpression::
    InternalSchemaAllElemMatchFromIndexMatchExpression(
        boost::optional<std::string_view> path,
        long long index,
        std::unique_ptr<ExpressionWithPlaceholder> expression,
        clonable_ptr<ErrorAnnotation> annotation)
    : ArrayMatchingMatchExpression(
          MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX, path, std::move(annotation)),
      _index(index),
      _expression(std::move(expression)) {}

std::unique_ptr<MatchExpression> InternalSchemaAllElemMatchFromIndexMatchExpression::clone() const {
    auto clone = std::make_unique<InternalSchemaAllElemMatchFromIndexMatchExpression>(
        path(), _index, _expression->clone(), _errorAnnotation);
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return clone;
}

bool InternalSchemaAllElemMatchFromIndexMatchExpression::equivalent(
    const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }
    const InternalSchemaAllElemMatchFromIndexMatchExpression* realOther =
        static_cast<const InternalSchemaAllElemMatchFromIndexMatchExpression*>(other);
    return (_index == realOther->_index && _expression->equivalent(realOther->_expression.get()));
}

void InternalSchemaAllElemMatchFromIndexMatchExpression::debugString(StringBuilder& debug,
                                                                     int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << kName;
    _debugStringAttachTagInfo(&debug);
    debug << " index: " << _index << ", query:\n";
    _expression->getFilter()->debugString(debug, indentationLevel + 1);
}

void InternalSchemaAllElemMatchFromIndexMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    bob->append(kName,
                BSON_ARRAY(opts.serializeLiteral(_index)
                           << _expression->getFilter()->serialize(opts, includePath)));
}

}  //  namespace mongo

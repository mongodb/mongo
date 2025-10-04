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

#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/value.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
constexpr StringData InternalSchemaMatchArrayIndexMatchExpression::kName;

InternalSchemaMatchArrayIndexMatchExpression::InternalSchemaMatchArrayIndexMatchExpression(
    boost::optional<StringData> path,
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
    BSONObjBuilder* bob, const SerializationOptions& opts, bool includePath) const {
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

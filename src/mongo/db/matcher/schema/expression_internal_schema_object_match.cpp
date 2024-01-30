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

#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"

namespace mongo {

constexpr StringData InternalSchemaObjectMatchExpression::kName;

InternalSchemaObjectMatchExpression::InternalSchemaObjectMatchExpression(
    boost::optional<StringData> path,
    std::unique_ptr<MatchExpression> expr,
    clonable_ptr<ErrorAnnotation> annotation)
    : PathMatchExpression(INTERNAL_SCHEMA_OBJECT_MATCH,
                          path,
                          ElementPath::LeafArrayBehavior::kNoTraversal,
                          ElementPath::NonLeafArrayBehavior::kTraverse,
                          std::move(annotation)),
      _sub(std::move(expr)) {}

bool InternalSchemaObjectMatchExpression::matchesSingleElement(const BSONElement& elem,
                                                               MatchDetails* details) const {
    if (elem.type() != BSONType::Object) {
        return false;
    }
    return _sub->matchesBSON(elem.Obj());
}

void InternalSchemaObjectMatchExpression::debugString(StringBuilder& debug,
                                                      int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << kName;
    _debugStringAttachTagInfo(&debug);
    _sub->debugString(debug, indentationLevel + 1);
}

BSONObj InternalSchemaObjectMatchExpression::getSerializedRightHandSide(
    SerializationOptions opts) const {
    return BSON(kName << _sub->serialize(opts));
}

bool InternalSchemaObjectMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }

    return _sub->equivalent(other->getChild(0));
}

std::unique_ptr<MatchExpression> InternalSchemaObjectMatchExpression::clone() const {
    auto clone = std::make_unique<InternalSchemaObjectMatchExpression>(
        path(), _sub->clone(), _errorAnnotation);
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return std::move(clone);
}

MatchExpression::ExpressionOptimizerFunc InternalSchemaObjectMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        auto& objectMatchExpression =
            static_cast<InternalSchemaObjectMatchExpression&>(*expression);
        objectMatchExpression._sub =
            MatchExpression::optimize(std::move(objectMatchExpression._sub));

        return expression;
    };
}
}  // namespace mongo

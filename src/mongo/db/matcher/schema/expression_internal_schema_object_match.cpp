// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/path.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

constexpr std::string_view InternalSchemaObjectMatchExpression::kName;

InternalSchemaObjectMatchExpression::InternalSchemaObjectMatchExpression(
    boost::optional<std::string_view> path,
    std::unique_ptr<MatchExpression> expr,
    clonable_ptr<ErrorAnnotation> annotation)
    : PathMatchExpression(INTERNAL_SCHEMA_OBJECT_MATCH,
                          path,
                          ElementPath::LeafArrayBehavior::kNoTraversal,
                          ElementPath::NonLeafArrayBehavior::kTraverse,
                          std::move(annotation)),
      _sub(std::move(expr)) {}

void InternalSchemaObjectMatchExpression::debugString(StringBuilder& debug,
                                                      int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << kName;
    _debugStringAttachTagInfo(&debug);
    _sub->debugString(debug, indentationLevel + 1);
}

void InternalSchemaObjectMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    bob->append(kName, _sub->serialize(opts, includePath));
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

}  // namespace mongo

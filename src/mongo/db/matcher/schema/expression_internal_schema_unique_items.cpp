// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {
constexpr std::string_view InternalSchemaUniqueItemsMatchExpression::kName;

void InternalSchemaUniqueItemsMatchExpression::debugString(StringBuilder& debug,
                                                           int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    BSONObjBuilder builder;
    serialize(&builder, {});
    debug << builder.obj().toString();
    _debugStringAttachTagInfo(&debug);
}

bool InternalSchemaUniqueItemsMatchExpression::equivalent(const MatchExpression* expr) const {
    if (matchType() != expr->matchType()) {
        return false;
    }

    const auto* other = static_cast<const InternalSchemaUniqueItemsMatchExpression*>(expr);
    return path() == other->path();
}

void InternalSchemaUniqueItemsMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    bob->append(kName, true);
}

std::unique_ptr<MatchExpression> InternalSchemaUniqueItemsMatchExpression::clone() const {
    auto clone =
        std::make_unique<InternalSchemaUniqueItemsMatchExpression>(path(), _errorAnnotation);
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return clone;
}
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_num_array_items.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/value.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

InternalSchemaNumArrayItemsMatchExpression::InternalSchemaNumArrayItemsMatchExpression(
    MatchType type,
    boost::optional<std::string_view> path,
    long long numItems,
    std::string_view name,
    clonable_ptr<ErrorAnnotation> annotation)
    : ArrayMatchingMatchExpression(type, path, std::move(std::move(annotation))),
      _name(name),
      _numItems(numItems) {}

void InternalSchemaNumArrayItemsMatchExpression::debugString(StringBuilder& debug,
                                                             int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " " << _name << " " << _numItems;
    _debugStringAttachTagInfo(&debug);
}

void InternalSchemaNumArrayItemsMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    opts.appendLiteral(bob, _name, _numItems);
}

bool InternalSchemaNumArrayItemsMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const InternalSchemaNumArrayItemsMatchExpression* realOther =
        static_cast<const InternalSchemaNumArrayItemsMatchExpression*>(other);

    return path() == realOther->path() && _numItems == realOther->_numItems;
}
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_str_length.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/value.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

InternalSchemaStrLengthMatchExpression::InternalSchemaStrLengthMatchExpression(
    MatchType type,
    boost::optional<std::string_view> path,
    long long strLen,
    std::string_view name,
    clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(type, path, std::move(annotation)), _name(name), _strLen(strLen) {}

void InternalSchemaStrLengthMatchExpression::debugString(StringBuilder& debug,
                                                         int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " " << _name << " " << _strLen;
    _debugStringAttachTagInfo(&debug);
}

void InternalSchemaStrLengthMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    opts.appendLiteral(bob, _name, _strLen);
}

bool InternalSchemaStrLengthMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const InternalSchemaStrLengthMatchExpression* realOther =
        static_cast<const InternalSchemaStrLengthMatchExpression*>(other);

    return path() == realOther->path() && _strLen == realOther->_strLen;
}

}  // namespace mongo

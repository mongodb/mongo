// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_num_properties.h"

#include "mongo/bson/bsonobj.h"

namespace mongo {

void InternalSchemaNumPropertiesMatchExpression::debugString(StringBuilder& debug,
                                                             int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    BSONObjBuilder builder;
    serialize(&builder, {});
    debug << builder.obj().toString();
    _debugStringAttachTagInfo(&debug);
}

void InternalSchemaNumPropertiesMatchExpression::serialize(
    BSONObjBuilder* out, const query_shape::SerializationOptions& opts, bool includePath) const {
    opts.appendLiteral(out, _name, _numProperties);
}

bool InternalSchemaNumPropertiesMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;
    const InternalSchemaNumPropertiesMatchExpression* otherMaxProperties =
        static_cast<const InternalSchemaNumPropertiesMatchExpression*>(other);
    return _numProperties == otherMaxProperties->_numProperties;
}
}  // namespace mongo

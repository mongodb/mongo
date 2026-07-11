// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <string_view>

namespace mongo {
constexpr std::string_view InternalSchemaXorMatchExpression::kName;

void InternalSchemaXorMatchExpression::debugString(StringBuilder& debug,
                                                   int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << kName;
    _debugStringAttachTagInfo(&debug);
    _debugList(debug, indentationLevel);
}

void InternalSchemaXorMatchExpression::serialize(BSONObjBuilder* out,
                                                 const query_shape::SerializationOptions& opts,
                                                 bool includePath) const {
    BSONArrayBuilder arrBob(out->subarrayStart(kName));
    _listToBSON(&arrBob, opts, includePath);
}
}  //  namespace mongo

/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape.h"
#include "query_request_helper.h"
#include "sort_pattern.h"

namespace mongo::query_shape {

BSONObj debugPredicateShape(const MatchExpression* predicate) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    return predicate->serialize(opts);
}
BSONObj representativePredicateShape(const MatchExpression* predicate) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue;
    return predicate->serialize(opts);
}

BSONObj debugPredicateShape(const MatchExpression* predicate,
                            std::function<std::string(StringData)> identifierRedactionPolicy) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    opts.identifierRedactionPolicy = identifierRedactionPolicy;
    opts.redactIdentifiers = true;
    return predicate->serialize(opts);
}

BSONObj representativePredicateShape(
    const MatchExpression* predicate,
    std::function<std::string(StringData)> identifierRedactionPolicy) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue;
    opts.identifierRedactionPolicy = identifierRedactionPolicy;
    opts.redactIdentifiers = true;
    return predicate->serialize(opts);
}

BSONObj sortShape(const BSONObj& sortSpec,
                  const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const SerializationOptions& opts) {
    if (sortSpec.isEmpty()) {
        return sortSpec;
    }
    auto natural = sortSpec[query_request_helper::kNaturalSortField];

    if (!natural) {
        return SortPattern{sortSpec, expCtx}
            .serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts)
            .toBson();
    }
    // This '$natural' will fail to parse as a valid SortPattern since it is not a valid field
    // path - it is usually considered and converted into a hint. For the query shape, we'll
    // keep it unmodified.
    BSONObjBuilder bob;
    for (auto&& elem : sortSpec) {
        if (elem.isABSONObj()) {
            // We expect this won't work or parse on the main command path, but for shapification we
            // don't really care, just treat it as a literal and don't bother parsing.
            bob << opts.serializeFieldPathFromString(elem.fieldNameStringData())
                << kLiteralArgString;
        } else if (elem.fieldNameStringData() == natural.fieldNameStringData()) {
            bob.append(elem);
        } else {
            bob.appendAs(elem, opts.serializeFieldPathFromString(elem.fieldNameStringData()));
        }
    }
    return bob.obj();
}

}  // namespace mongo::query_shape

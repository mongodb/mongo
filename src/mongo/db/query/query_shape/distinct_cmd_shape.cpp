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

#include "mongo/db/query/query_shape/distinct_cmd_shape.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/pipeline/expression_context_builder.h"

namespace mongo::query_shape {

DistinctCmdShapeComponents::DistinctCmdShapeComponents(
    const ParsedDistinctCommand& request, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : key(std::string{request.distinctCommandRequest->getKey()}),
      representativeQuery(request.query->serialize(
          SerializationOptions::kRepresentativeQueryShapeSerializeOptions)) {}

void DistinctCmdShapeComponents::HashValue(absl::HashState state) const {
    absl::HashState::combine(std::move(state), key, simpleHash(representativeQuery));
}

size_t DistinctCmdShapeComponents::size() const {
    return sizeof(DistinctCmdShapeComponents) + key.size() + representativeQuery.objsize();
}

const CmdSpecificShapeComponents& DistinctCmdShape::specificComponents() const {
    return components;
}

DistinctCmdShape::DistinctCmdShape(const ParsedDistinctCommand& distinct,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(distinct.distinctCommandRequest->getNamespaceOrUUID(),
            distinct.distinctCommandRequest->getCollation().get_value_or(BSONObj())),
      components(distinct, expCtx) {}

void DistinctCmdShape::appendCmdSpecificShapeComponents(BSONObjBuilder& bob,
                                                        OperationContext* opCtx,
                                                        const SerializationOptions& opts) const {
    // Command name.
    bob.append("command", DistinctCommandRequest::kCommandName);

    // Key.
    bob.append(DistinctCommandRequest::kKeyFieldName,
               opts.serializeFieldPathFromString(components.key));

    // Query.
    if (!components.representativeQuery.isEmpty()) {
        if (opts == SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
            // Fastpath. Already serialized using the same serialization options.
            bob.append(DistinctCommandRequest::kQueryFieldName, components.representativeQuery);
        } else {
            auto expCtx = makeBlankExpressionContext(opCtx, nssOrUUID);
            auto matchExpr = uassertStatusOK(
                MatchExpressionParser::parse(components.representativeQuery,
                                             expCtx,
                                             ExtensionsCallbackNoop(),
                                             MatchExpressionParser::kAllowAllSpecialFeatures));
            bob.append(DistinctCommandRequest::kQueryFieldName, matchExpr->serialize(opts));
        }
    }
}

QueryShapeHash DistinctCmdShape::sha256Hash(OperationContext*, const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "distinct" command shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> distinctCommandShapeBuffer;

    // Write small or typically empty "distinct" command shape parts to the buffer.
    distinctCommandShapeBuffer.appendStrBytes(DistinctCommandRequest::kCommandName);

    // 16-bit command options word. Use 0th bit as an indicator whether the command specification
    // includes a namespace or a UUID of a collection. The remaining bits are reserved for encoding
    // command options in the future.
    const std::uint16_t commandOptions = nssOrUUID.isNamespaceString() ? 0 : 1;
    distinctCommandShapeBuffer.appendNum(static_cast<short>(commandOptions));
    auto nssDataRange = nssOrUUID.asDataRange();
    distinctCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());

    // Append a null byte to separate the namespace string 'nssOrUUID' from the "key" parameter
    // value.
    distinctCommandShapeBuffer.appendChar(0);
    distinctCommandShapeBuffer.appendCStr(components.key);
    distinctCommandShapeBuffer.appendBuf(collation.objdata(), collation.objsize());
    return SHA256Block::computeHash({
        ConstDataRange{distinctCommandShapeBuffer.buf(),
                       static_cast<std::size_t>(distinctCommandShapeBuffer.len())},
        components.representativeQuery.asDataRange(),
    });
}
}  // namespace mongo::query_shape

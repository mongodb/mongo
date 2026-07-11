// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/distinct_cmd_shape.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/pipeline/expression_context_builder.h"

namespace mongo::query_shape {

DistinctCmdShapeComponents::DistinctCmdShapeComponents(
    const ParsedDistinctCommand& request, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : key(std::string{request.distinctCommandRequest->getKey()}),
      representativeQuery(request.query->serialize(
          query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions)) {}

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

void DistinctCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    // Command name.
    bob.append("command", DistinctCommandRequest::kCommandName);

    // Key.
    bob.append(DistinctCommandRequest::kKeyFieldName,
               opts.serializeFieldPathFromString(components.key));

    // Query.
    if (!components.representativeQuery.isEmpty()) {
        if (opts == query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
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

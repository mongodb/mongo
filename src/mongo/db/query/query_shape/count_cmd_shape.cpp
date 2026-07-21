// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/count_cmd_shape.h"

#include "mongo/db/pipeline/expression_context_builder.h"

namespace mongo::query_shape {

CountCmdShapeComponents::CountCmdShapeComponents(const ParsedFindCommand& request,
                                                 const bool hasLimit,
                                                 const bool hasSkip)
    : hasField({.limit = hasLimit, .skip = hasSkip}),
      representativeQuery(request.filter->serialize(
          query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions)) {}

void CountCmdShapeComponents::HashValue(absl::HashState state) const {
    absl::HashState::combine(
        std::move(state), simpleHash(representativeQuery), hasField.limit, hasField.skip);
}

size_t CountCmdShapeComponents::size() const {
    return sizeof(CountCmdShapeComponents) + representativeQuery.objsize();
}

CountCmdShape::CountCmdShape(const ParsedFindCommand& find,
                             const bool hasLimit,
                             const bool hasSkip,
                             const bool rawData)
    : Shape(find.findCommandRequest->getNamespaceOrUUID(),
            find.findCommandRequest->getCollation(),
            rawData),
      components(find, hasLimit, hasSkip) {}

const CmdSpecificShapeComponents& CountCmdShape::specificComponents() const {
    return components;
}

void CountCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    tassert(9065200,
            "Serialization policy not supported - original values have been discarded",
            !opts.isKeepingLiteralsUnchanged());

    // Command name.
    bob.append("command", CountCommandRequest::kCommandName);

    // Query.
    // Query field is optional and thus can be empty.
    if (!components.representativeQuery.isEmpty()) {
        if (opts == query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
            // Fast path. Already serialized using the same serialization options.
            bob.append(CountCommandRequest::kQueryFieldName, components.representativeQuery);
        } else {
            // Slow path: We need to re-parse from our representative shapes.
            auto expCtx = makeBlankExpressionContext(opCtx, nssOrUUID);
            auto matchExpr = uassertStatusOK(
                MatchExpressionParser::parse(components.representativeQuery,
                                             expCtx,
                                             ExtensionsCallbackNoop(),
                                             MatchExpressionParser::kAllowAllSpecialFeatures));
            // Serialize representative query to debug query using options
            bob.append(CountCommandRequest::kQueryFieldName, matchExpr->serialize(opts));
        }
    }

    // Skip and limit.
    if (components.hasField.limit) {
        opts.appendLiteral(&bob, CountCommandRequest::kLimitFieldName, 1ll);
    }
    if (components.hasField.skip) {
        opts.appendLiteral(&bob, CountCommandRequest::kSkipFieldName, 1ll);
    }
}

QueryShapeHash CountCmdShape::sha256Hash(OperationContext*, const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "count" command shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> countCommandShapeBuffer;

    // Write small or typically empty "count" command shape parts to the buffer.
    countCommandShapeBuffer.appendStrBytes(CountCommandRequest::kCommandName);

    // 16-bit command options word. Use 0th bit as an indicator whether the command specification
    // includes a namespace or a UUID of a collection. The remaining bits are reserved for encoding
    // command-specific options in the future.
    countCommandShapeBuffer.appendNum(static_cast<short>(nssOrUUID.isNamespaceString() ? 0 : 1));

    // Common command options (e.g. rawData) are appended as a separate word, and only when one of
    // them is set, so that commands without any common options keep their historical hashes. See
    // Shape::commonOptionsWord() for the bit layout.
    if (const auto commonOptions = commonOptionsWord()) {
        countCommandShapeBuffer.appendNum(static_cast<short>(commonOptions));
    }

    auto nssDataRange = nssOrUUID.asDataRange();
    countCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());
    countCommandShapeBuffer.appendBuf(collation.objdata(), collation.objsize());

    // Encode whether or not skip and limit are included in command.
    std::uint8_t skipAndLimit{0};
    skipAndLimit |= static_cast<uint8_t>(components.hasField.skip);
    skipAndLimit |= static_cast<uint8_t>(components.hasField.limit) << 1;
    countCommandShapeBuffer.appendNum(static_cast<char>(skipAndLimit));

    return SHA256Block::computeHash({
        ConstDataRange{countCommandShapeBuffer.buf(),
                       static_cast<std::size_t>(countCommandShapeBuffer.len())},
        components.representativeQuery.asDataRange(),
    });
}
}  // namespace mongo::query_shape

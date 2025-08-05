/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/count_cmd_shape.h"

#include "mongo/db/pipeline/expression_context_builder.h"

namespace mongo::query_shape {

CountCmdShapeComponents::CountCmdShapeComponents(const ParsedFindCommand& request,
                                                 const bool hasLimit,
                                                 const bool hasSkip)
    : hasField({.limit = hasLimit, .skip = hasSkip}),
      representativeQuery(request.filter->serialize(
          SerializationOptions::kRepresentativeQueryShapeSerializeOptions)) {}

void CountCmdShapeComponents::HashValue(absl::HashState state) const {
    absl::HashState::combine(
        std::move(state), simpleHash(representativeQuery), hasField.limit, hasField.skip);
}

size_t CountCmdShapeComponents::size() const {
    return sizeof(CountCmdShapeComponents) + representativeQuery.objsize();
}

CountCmdShape::CountCmdShape(const ParsedFindCommand& find, const bool hasLimit, const bool hasSkip)
    : Shape(find.findCommandRequest->getNamespaceOrUUID(), find.findCommandRequest->getCollation()),
      components(find, hasLimit, hasSkip) {}

const CmdSpecificShapeComponents& CountCmdShape::specificComponents() const {
    return components;
}

void CountCmdShape::appendCmdSpecificShapeComponents(BSONObjBuilder& bob,
                                                     OperationContext* opCtx,
                                                     const SerializationOptions& opts) const {
    tassert(9065200,
            "Serialization policy not supported - original values have been discarded",
            !opts.isKeepingLiteralsUnchanged());

    // Command name.
    bob.append("command", CountCommandRequest::kCommandName);

    // Query.
    // Query field is optional and thus can be empty.
    if (!components.representativeQuery.isEmpty()) {
        if (opts == SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
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
    // command options in the future.
    const std::uint16_t commandOptions = nssOrUUID.isNamespaceString() ? 0 : 1;
    countCommandShapeBuffer.appendNum(static_cast<short>(commandOptions));

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

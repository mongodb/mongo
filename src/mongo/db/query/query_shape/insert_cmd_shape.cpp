// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/insert_cmd_shape.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_shape/serialization_options.h"

namespace mongo::query_shape {

void InsertCmdShapeComponents::appendTo(BSONObjBuilder& bob,
                                        const query_shape::SerializationOptions& opts) const {
    bob.append("command", "insert");

    // 'documents' is always shapified as ?array<?object>: a placeholder array of one empty object.
    // We create a backing BSON object so we can extract a BSONElement of array type for
    // appendLiteral.
    static const BSONObj kDocumentPlaceholderBacking = BSON("documents" << BSON_ARRAY(BSONObj()));
    opts.appendLiteral(&bob, kDocumentPlaceholderBacking.firstElement());
}

InsertCmdShape::InsertCmdShape(const write_ops::InsertCommandRequest& request)
    : Shape(request.getNamespace(), BSONObj{} /*no collation for insert*/) {}

void InsertCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    _components.appendTo(bob, opts);
}

QueryShapeHash InsertCmdShape::sha256Hash(OperationContext*, const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "insert" command shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> insertCommandShapeBuffer;

    tassert(12205900,
            "nssOrUUID for an insert must be a namespace string",
            nssOrUUID.isNamespaceString());
    auto nssDataRange = nssOrUUID.asDataRange();

    // Write the two relevant "insert" shape parts to the buffer ("insert" and namespace). The
    // documents field is not included in the hash, as it is always shapified as the same
    // placeholder array and thus, doesn't provide any value in differentiating insert query shapes.
    insertCommandShapeBuffer.appendStrBytes(write_ops::InsertCommandRequest::kCommandName);
    insertCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());

    return SHA256Block::computeHash({ConstDataRange{
        insertCommandShapeBuffer.buf(), static_cast<std::size_t>(insertCommandShapeBuffer.len())}});
}

}  // namespace mongo::query_shape

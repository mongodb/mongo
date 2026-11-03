// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/delete_cmd_shape.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/write_component_serialization.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"

#include <absl/hash/hash.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_shape {


DeleteCmdShapeComponents::DeleteCmdShapeComponents(const ParsedDelete& parsedDelete,
                                                   LetShapeComponent let,
                                                   const query_shape::SerializationOptions& opts)
    : representativeQ(serializeWriteQuery(parsedDelete, opts)),
      multi(parsedDelete.getRequest()->getMulti()),
      let(let) {}

void DeleteCmdShapeComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), simpleHash(representativeQ), multi, let);
}

void DeleteCmdShapeComponents::appendTo(
    BSONObjBuilder& bob,
    const query_shape::SerializationOptions& opts,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    bob.append("command", "delete");

    bob.append(write_ops::DeleteOpEntry::kQFieldName, representativeQ);
    write_ops::writeMultiDeleteProperty(multi, write_ops::DeleteOpEntry::kMultiFieldName, &bob);

    let.appendTo(bob, opts, expCtx);
}

size_t DeleteCmdShapeComponents::size() const {
    return sizeof(DeleteCmdShapeComponents) + representativeQ.objsize() + let.size() -
        sizeof(LetShapeComponent);
}

DeleteCmdShape::DeleteCmdShape(const ParsedDelete& parsedDelete,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(parsedDelete.getRequest()->getNsString(),
            parsedDelete.getRequest()->getCollation().getOwned()),
      _components(parsedDelete, LetShapeComponent(parsedDelete.getRequest()->getLet(), expCtx)) {}

const CmdSpecificShapeComponents& DeleteCmdShape::specificComponents() const {
    return _components;
}

size_t DeleteCmdShape::extraSize() const {
    return sizeof(DeleteCmdShape) - sizeof(Shape) - sizeof(DeleteCmdShapeComponents);
}

void DeleteCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    tassert(12205300,
            "We don't support serializing to the unmodified shape here, since we have already "
            "shapified and stored the representative query - we've lost the original literals",
            !opts.isKeepingLiteralsUnchanged());

    auto expCtx = makeBlankExpressionContext(opCtx, nssOrUUID, _components.let.shapifiedLet);
    if (opts == query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // We have this copy stored already!
        _components.appendTo(bob, opts, expCtx);
        return;
    }

    // Slow path: we need to re-parse from our representative shapes and re-shapify with 'opts'.
    expCtx->setIsReparsingRepresentativeQueryShape(true);

    DeleteRequest deleteRequest;
    tassert(12205301,
            "nssOrUUID for a delete must be a namespace string",
            nssOrUUID.isNamespaceString());
    deleteRequest.setNsString(nssOrUUID.nss());
    deleteRequest.setQuery(_components.representativeQ);
    deleteRequest.setMulti(_components.multi);
    if (!collation.isEmpty()) {
        deleteRequest.setCollation(collation.getOwned());
    }
    if (_components.let.hasLet) {
        deleteRequest.setLet(_components.let.shapifiedLet);
    }

    auto parsedDelete = uassertStatusOK(parsed_delete_command::parse(
        expCtx, &deleteRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

    DeleteCmdShapeComponents{parsedDelete, _components.let, opts}.appendTo(bob, opts, expCtx);
}

QueryShapeHash DeleteCmdShape::sha256Hash(OperationContext*, const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "delete" command shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> deleteCommandShapeBuffer;

    // Write small or typically empty "delete" command shape parts to the buffer.
    deleteCommandShapeBuffer.appendStrBytes(write_ops::DeleteCommandRequest::kCommandName);
    deleteCommandShapeBuffer.appendNum(static_cast<int>(_components.multi));

    tassert(12206000,
            "nssOrUUID for a delete must be a namespace string",
            nssOrUUID.isNamespaceString());
    auto nssDataRange = nssOrUUID.asDataRange();
    deleteCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());

    deleteCommandShapeBuffer.appendBuf(collation.objdata(), collation.objsize());

    return SHA256Block::computeHash(
        {ConstDataRange{deleteCommandShapeBuffer.buf(),
                        static_cast<std::size_t>(deleteCommandShapeBuffer.len())},
         _components.representativeQ.asDataRange(),
         _components.let.shapifiedLet.asDataRange()});
}
}  // namespace mongo::query_shape

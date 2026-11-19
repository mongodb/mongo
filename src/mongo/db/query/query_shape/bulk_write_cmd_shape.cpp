// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/bulk_write_cmd_shape.h"

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"

namespace mongo::query_shape {
namespace {

// The 'op' field of a bulkWrite op's query shape carries the op type.
constexpr auto kOpFieldName = "op"sv;

}  // namespace

// Uses the bulkWrite field names ('filter', 'updateMods') instead of the standalone ('q', 'u').
template <>
void BulkWriteUpdateCmdShapeComponents::appendTo(
    BSONObjBuilder& bob,
    const query_shape::SerializationOptions& opts,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    bob.append("command", BulkWriteCommandRequest::kCommandName);
    bob.append(kOpFieldName, kBulkWriteUpdateOpName);

    bob.append(BulkWriteUpdateOp::kFilterFieldName, standalone.representativeQ);
    bob.appendAs(standalone.getRepresentativeU(), BulkWriteUpdateOp::kUpdateModsFieldName);

    if (standalone.representativeC.has_value()) {
        bob.append(BulkWriteUpdateOp::kConstantsFieldName, *standalone.representativeC);
    }
    if (standalone.representativeArrayFilters.has_value()) {
        bob.append(BulkWriteUpdateOp::kArrayFiltersFieldName,
                   *standalone.representativeArrayFilters);
    }

    bob.append(BulkWriteUpdateOp::kMultiFieldName, bool(standalone.multi));
    bob.append(BulkWriteUpdateOp::kUpsertFieldName, bool(standalone.upsert));

    standalone.let.appendTo(bob, opts, expCtx);
}

// Uses the bulkWrite field name 'filter' instead of the standalone 'q'.
template <>
void BulkWriteDeleteCmdShapeComponents::appendTo(
    BSONObjBuilder& bob,
    const query_shape::SerializationOptions& opts,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    bob.append("command", BulkWriteCommandRequest::kCommandName);
    bob.append(kOpFieldName, kBulkWriteDeleteOpName);

    bob.append(BulkWriteDeleteOp::kFilterFieldName, standalone.representativeQ);
    bob.append(BulkWriteDeleteOp::kMultiFieldName, bool(standalone.multi));

    standalone.let.appendTo(bob, opts, expCtx);
}

BulkWriteInsertCmdShape::BulkWriteInsertCmdShape(const NamespaceString& nss)
    : Shape(nss, BSONObj{} /*no collation for insert*/) {}

void BulkWriteInsertCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    bob.append("command", BulkWriteCommandRequest::kCommandName);
    bob.append(kOpFieldName, kBulkWriteInsertOpName);

    // 'documents' is always shapified as ?array<?object>: a placeholder array of one empty object,
    // exactly like the standalone insert command's shape.
    static const BSONObj kDocumentPlaceholderBacking = BSON("documents" << BSON_ARRAY(BSONObj()));
    opts.appendLiteral(&bob, kDocumentPlaceholderBacking.firstElement());
}

QueryShapeHash BulkWriteInsertCmdShape::sha256Hash(OperationContext*,
                                                   const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "bulkWrite" insert op shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> insertCommandShapeBuffer;

    tassert(13879400,
            "nssOrUUID for a bulkWrite insert op must be a namespace string",
            nssOrUUID.isNamespaceString());
    auto nssDataRange = nssOrUUID.asDataRange();

    // "bulkWrite" and 'op' are hashed only to tell this apart from the standalone insert, which
    // likewise leaves out 'documents' because it always shapifies to the same placeholder.
    insertCommandShapeBuffer.appendStrBytes(BulkWriteCommandRequest::kCommandName);
    insertCommandShapeBuffer.appendStrBytes(kBulkWriteInsertOpName);
    insertCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());

    return SHA256Block::computeHash({ConstDataRange{
        insertCommandShapeBuffer.buf(), static_cast<std::size_t>(insertCommandShapeBuffer.len())}});
}

BulkWriteUpdateCmdShape::BulkWriteUpdateCmdShape(
    const ParsedUpdate& parsedUpdate, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(parsedUpdate.getRequest()->getNamespaceString(),
            parsedUpdate.getRequest()->getCollation().getOwned()),
      _components(parsedUpdate,
                  LetShapeComponent(parsedUpdate.getRequest()->getLetParameters(), expCtx)) {}

void BulkWriteUpdateCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    tassert(13879401,
            "We don't support serializing to the unmodified shape here, since we have already "
            "shapified and stored the representative query - we've lost the original literals",
            !opts.isKeepingLiteralsUnchanged());

    auto expCtx =
        makeBlankExpressionContext(opCtx, nssOrUUID, _components.standalone.let.shapifiedLet);
    if (opts == query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // We have this copy stored already!
        _components.appendTo(bob, opts, expCtx);
        return;
    }

    // Slow path: we need to re-parse from our representative shapes and re-shapify with 'opts'.
    expCtx->setIsReparsingRepresentativeQueryShape(true);
    auto updateRequest = makeRepresentativeUpdateRequest(nssOrUUID, _components.standalone);
    auto parsedUpdate = uassertStatusOK(parsed_update_command::parse(
        expCtx, &updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

    BulkWriteUpdateCmdShapeComponents{parsedUpdate, _components.standalone.let, opts}.appendTo(
        bob, opts, expCtx);
}

QueryShapeHash BulkWriteUpdateCmdShape::sha256Hash(OperationContext*,
                                                   const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "bulkWrite" update op shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> updateCommandShapeBuffer;

    // Write small or typically empty "bulkWrite" update op shape parts to the buffer.
    updateCommandShapeBuffer.appendStrBytes(BulkWriteCommandRequest::kCommandName);
    updateCommandShapeBuffer.appendStrBytes(kBulkWriteUpdateOpName);

    // Append bits corresponding to the optional multi and upsert fields, representativeC, and
    // representativeArrayFilters fields.
    updateCommandShapeBuffer.appendNum(
        static_cast<int>(_components.standalone.multi) << 3 |
        static_cast<int>(_components.standalone.upsert) << 2 |
        static_cast<int>(_components.standalone.representativeC.has_value()) << 1 |
        static_cast<int>(_components.standalone.representativeArrayFilters.has_value()));

    tassert(13879402,
            "nssOrUUID for a bulkWrite update op must be a namespace string",
            nssOrUUID.isNamespaceString());
    auto nssDataRange = nssOrUUID.asDataRange();
    updateCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());
    updateCommandShapeBuffer.appendBuf(collation.objdata(), collation.objsize());
    if (_components.standalone.representativeArrayFilters) {
        const auto& filters = *_components.standalone.representativeArrayFilters;
        updateCommandShapeBuffer.appendNum(static_cast<uint32_t>(filters.size()));
        for (const auto& filter : filters) {
            updateCommandShapeBuffer.appendBuf(filter.objdata(), filter.objsize());
        }
    }

    BSONObj representativeC = _components.standalone.representativeC.value_or(BSONObj{});
    BSONObj representativeU = _components.standalone.getRepresentativeU().Obj();
    return SHA256Block::computeHash(
        {ConstDataRange{updateCommandShapeBuffer.buf(),
                        static_cast<std::size_t>(updateCommandShapeBuffer.len())},
         representativeU.asDataRange(),
         _components.standalone.representativeQ.asDataRange(),
         _components.standalone.let.shapifiedLet.asDataRange(),
         representativeC.asDataRange()});
}

BulkWriteDeleteCmdShape::BulkWriteDeleteCmdShape(
    const ParsedDelete& parsedDelete, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(parsedDelete.getRequest()->getNsString(),
            parsedDelete.getRequest()->getCollation().getOwned()),
      _components(parsedDelete, LetShapeComponent(parsedDelete.getRequest()->getLet(), expCtx)) {}

void BulkWriteDeleteCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    tassert(13879403,
            "We don't support serializing to the unmodified shape here, since we have already "
            "shapified and stored the representative query - we've lost the original literals",
            !opts.isKeepingLiteralsUnchanged());

    auto expCtx =
        makeBlankExpressionContext(opCtx, nssOrUUID, _components.standalone.let.shapifiedLet);
    if (opts == query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // We have this copy stored already!
        _components.appendTo(bob, opts, expCtx);
        return;
    }

    // Slow path: we need to re-parse from our representative shapes and re-shapify with 'opts'.
    expCtx->setIsReparsingRepresentativeQueryShape(true);
    auto deleteRequest =
        makeRepresentativeDeleteRequest(nssOrUUID, _components.standalone, collation);
    auto parsedDelete = uassertStatusOK(parsed_delete_command::parse(
        expCtx, &deleteRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

    BulkWriteDeleteCmdShapeComponents{parsedDelete, _components.standalone.let, opts}.appendTo(
        bob, opts, expCtx);
}

QueryShapeHash BulkWriteDeleteCmdShape::sha256Hash(OperationContext*,
                                                   const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "bulkWrite" delete op shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> deleteCommandShapeBuffer;

    // Write small or typically empty "bulkWrite" delete op shape parts to the buffer.
    deleteCommandShapeBuffer.appendStrBytes(BulkWriteCommandRequest::kCommandName);
    deleteCommandShapeBuffer.appendStrBytes(kBulkWriteDeleteOpName);
    deleteCommandShapeBuffer.appendNum(static_cast<int>(_components.standalone.multi));

    tassert(13879404,
            "nssOrUUID for a bulkWrite delete op must be a namespace string",
            nssOrUUID.isNamespaceString());
    auto nssDataRange = nssOrUUID.asDataRange();
    deleteCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());

    deleteCommandShapeBuffer.appendBuf(collation.objdata(), collation.objsize());

    return SHA256Block::computeHash(
        {ConstDataRange{deleteCommandShapeBuffer.buf(),
                        static_cast<std::size_t>(deleteCommandShapeBuffer.len())},
         _components.standalone.representativeQ.asDataRange(),
         _components.standalone.let.shapifiedLet.asDataRange()});
}

}  // namespace mongo::query_shape

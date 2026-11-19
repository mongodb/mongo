// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/write_cmd_shape_test_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/commands/query_cmd/bulk_write_common.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_shape/bulk_write_cmd_shape.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::query_shape {

UpdateRequest makeUpdateRequest(const write_ops::UpdateCommandRequest& cmd, size_t opIdx) {
    UpdateRequest request(cmd.getUpdates()[opIdx]);
    request.setNamespaceString(cmd.getNamespace());
    request.setLetParameters(cmd.getLet());
    return request;
}

DeleteRequest makeDeleteRequest(const write_ops::DeleteCommandRequest& cmd, size_t opIdx) {
    const auto& op = cmd.getDeletes()[opIdx];
    DeleteRequest request;
    request.setNsString(cmd.getNamespace());
    request.setQuery(op.getQ());
    request.setMulti(op.getMulti());
    request.setHint(op.getHint());
    request.setCollation(write_ops::collationOf(op));
    request.setLet(cmd.getLet());
    return request;
}

std::unique_ptr<UpdateCmdShape> makeUpdateShape(OperationContext* opCtx,
                                                const UpdateRequest& request) {
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, request).build();
    auto parsedUpdate = uassertStatusOK(parsed_update_command::parse(
        expCtx, &request, makeExtensionsCallback<ExtensionsCallbackNoop>()));
    return std::make_unique<UpdateCmdShape>(parsedUpdate, expCtx);
}

std::unique_ptr<DeleteCmdShape> makeDeleteShape(OperationContext* opCtx,
                                                const DeleteRequest& request) {
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, request).build();
    auto parsedDelete = uassertStatusOK(parsed_delete_command::parse(
        expCtx, &request, makeExtensionsCallback<ExtensionsCallbackNoop>()));
    return std::make_unique<DeleteCmdShape>(parsedDelete, expCtx);
}

std::vector<UpdateCmdShape> makeUpdateShapes(OperationContext* opCtx,
                                             const write_ops::UpdateCommandRequest& cmd) {
    std::vector<UpdateCmdShape> shapes;
    for (size_t opIdx = 0; opIdx < cmd.getUpdates().size(); ++opIdx) {
        shapes.push_back(*makeUpdateShape(opCtx, makeUpdateRequest(cmd, opIdx)));
    }
    return shapes;
}

std::vector<DeleteCmdShape> makeDeleteShapes(OperationContext* opCtx,
                                             const write_ops::DeleteCommandRequest& cmd) {
    std::vector<DeleteCmdShape> shapes;
    for (size_t opIdx = 0; opIdx < cmd.getDeletes().size(); ++opIdx) {
        shapes.push_back(*makeDeleteShape(opCtx, makeDeleteRequest(cmd, opIdx)));
    }
    return shapes;
}

UpdateCmdShape makeOneUpdateShape(OperationContext* opCtx, const BSONObj& updateCmd) {
    auto shapes =
        makeUpdateShapes(opCtx, write_ops::UpdateCommandRequest::parseOwned(updateCmd.getOwned()));
    ASSERT_EQ(shapes.size(), 1U);
    return shapes.front();
}

UpdateCmdShape makeOneUpdateShape(OperationContext* opCtx, std::string_view updateCmd) {
    return makeOneUpdateShape(opCtx, fromjson(updateCmd));
}

DeleteCmdShape makeOneDeleteShape(OperationContext* opCtx, const BSONObj& deleteCmd) {
    auto shapes =
        makeDeleteShapes(opCtx, write_ops::DeleteCommandRequest::parseOwned(deleteCmd.getOwned()));
    ASSERT_EQ(shapes.size(), 1U);
    return shapes.front();
}

DeleteCmdShape makeOneDeleteShape(OperationContext* opCtx, std::string_view deleteCmd) {
    return makeOneDeleteShape(opCtx, fromjson(deleteCmd));
}

InsertCmdShape makeInsertShape(std::string_view insertCmd) {
    return InsertCmdShape(
        write_ops::InsertCommandRequest::parseOwned(fromjson(insertCmd)).getNamespace());
}

UpdateRequest makeBulkWriteUpdateRequest(OperationContext* opCtx,
                                         const BulkWriteCommandRequest& cmd,
                                         size_t opIdx) {
    const auto& op = std::get<BulkWriteUpdateOp>(cmd.getOps()[opIdx]);
    return bulk_write_common::makeUpdateRequestFromUpdateOp(opCtx,
                                                            cmd.getNsInfo()[op.getNsInfoIdx()],
                                                            &op,
                                                            0 /* stmtId */,
                                                            cmd.getLet(),
                                                            cmd.getBypassEmptyTsReplacement());
}

DeleteRequest makeBulkWriteDeleteRequest(OperationContext* opCtx,
                                         const BulkWriteCommandRequest& cmd,
                                         size_t opIdx) {
    const auto& op = std::get<BulkWriteDeleteOp>(cmd.getOps()[opIdx]);
    return bulk_write_common::makeDeleteRequestFromDeleteOp(
        opCtx, cmd.getNsInfo()[op.getNsInfoIdx()], &op, 0 /* stmtId */, cmd.getLet());
}

std::unique_ptr<BulkWriteUpdateCmdShape> makeBulkWriteUpdateShape(OperationContext* opCtx,
                                                                  std::string_view bulkCmd,
                                                                  size_t opIdx) {
    auto cmd = BulkWriteCommandRequest::parseOwned(fromjson(bulkCmd));
    auto updateRequest = makeBulkWriteUpdateRequest(opCtx, cmd, opIdx);
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, updateRequest).build();
    auto parsedUpdate = uassertStatusOK(parsed_update_command::parse(
        expCtx, &updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));
    // The shape copies the representative BSONs, but its constructor dereferences the parsed
    // request, so 'updateRequest' must still be alive here.
    return std::make_unique<BulkWriteUpdateCmdShape>(parsedUpdate, expCtx);
}

std::unique_ptr<BulkWriteDeleteCmdShape> makeBulkWriteDeleteShape(OperationContext* opCtx,
                                                                  std::string_view bulkCmd,
                                                                  size_t opIdx) {
    auto cmd = BulkWriteCommandRequest::parseOwned(fromjson(bulkCmd));
    auto deleteRequest = makeBulkWriteDeleteRequest(opCtx, cmd, opIdx);
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, deleteRequest).build();
    auto parsedDelete = uassertStatusOK(parsed_delete_command::parse(
        expCtx, &deleteRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));
    // The shape copies the representative BSONs, but its constructor dereferences the parsed
    // request, so 'deleteRequest' must still be alive here.
    return std::make_unique<BulkWriteDeleteCmdShape>(parsedDelete, expCtx);
}

std::unique_ptr<BulkWriteInsertCmdShape> makeBulkWriteInsertShape(OperationContext* opCtx,
                                                                  std::string_view bulkCmd,
                                                                  size_t opIdx) {
    auto request = BulkWriteCommandRequest::parseOwned(fromjson(bulkCmd));
    const auto& op = std::get<BulkWriteInsertOp>(request.getOps()[opIdx]);
    return std::make_unique<BulkWriteInsertCmdShape>(
        request.getNsInfo()[op.getNsInfoIdx()].getNs());
}

std::unique_ptr<Shape> makeBulkWriteShape(OperationContext* opCtx,
                                          const BulkWriteCommandRequest& request,
                                          size_t opIdx) {
    return visit(
        OverloadedVisitor{
            [&](const BulkWriteInsertOp& op) -> std::unique_ptr<Shape> {
                return std::make_unique<BulkWriteInsertCmdShape>(
                    request.getNsInfo()[op.getNsInfoIdx()].getNs());
            },
            [&](const BulkWriteUpdateOp& op) -> std::unique_ptr<Shape> {
                auto updateRequest = makeBulkWriteUpdateRequest(opCtx, request, opIdx);
                auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, updateRequest).build();
                auto parsedUpdate = uassertStatusOK(parsed_update_command::parse(
                    expCtx, &updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));
                // The shape copies the representative BSONs, but its constructor dereferences the
                // parsed request, so 'updateRequest' must still be alive here.
                return std::make_unique<BulkWriteUpdateCmdShape>(parsedUpdate, expCtx);
            },
            [&](const BulkWriteDeleteOp& op) -> std::unique_ptr<Shape> {
                auto deleteRequest = makeBulkWriteDeleteRequest(opCtx, request, opIdx);
                auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, deleteRequest).build();
                auto parsedDelete = uassertStatusOK(parsed_delete_command::parse(
                    expCtx, &deleteRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));
                // The shape copies the representative BSONs, but its constructor dereferences the
                // parsed request, so 'deleteRequest' must still be alive here.
                return std::make_unique<BulkWriteDeleteCmdShape>(parsedDelete, expCtx);
            }},
        request.getOps()[opIdx]);
}

}  // namespace mongo::query_shape

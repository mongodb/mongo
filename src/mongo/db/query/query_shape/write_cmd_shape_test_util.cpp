// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/write_cmd_shape_test_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/unittest/unittest.h"

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

}  // namespace mongo::query_shape

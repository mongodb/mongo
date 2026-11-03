// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/write_cmd_shape_test_util.h"

#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"

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

}  // namespace mongo::query_shape

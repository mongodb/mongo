// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_shape/bulk_write_cmd_shape.h"
#include "mongo/db/query/query_shape/delete_cmd_shape.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/update_cmd_shape.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

namespace mongo::query_shape {

/**
 * Builds the per-op request for the op at 'opIdx' of a standalone update/delete command, the way
 * the write path does.
 */
UpdateRequest makeUpdateRequest(const write_ops::UpdateCommandRequest& cmd, size_t opIdx = 0);
DeleteRequest makeDeleteRequest(const write_ops::DeleteCommandRequest& cmd, size_t opIdx = 0);

/**
 * Parses 'request' and builds its shape. The shape does not reference 'request' afterwards.
 */
std::unique_ptr<UpdateCmdShape> makeUpdateShape(OperationContext* opCtx,
                                                const UpdateRequest& request);
std::unique_ptr<DeleteCmdShape> makeDeleteShape(OperationContext* opCtx,
                                                const DeleteRequest& request);

/**
 * Builds the shapes of all ops of a standalone update/delete command.
 */
std::vector<UpdateCmdShape> makeUpdateShapes(OperationContext* opCtx,
                                             const write_ops::UpdateCommandRequest& cmd);
std::vector<DeleteCmdShape> makeDeleteShapes(OperationContext* opCtx,
                                             const write_ops::DeleteCommandRequest& cmd);

/**
 * Builds the shape of a standalone command that must contain exactly one op.
 */
UpdateCmdShape makeOneUpdateShape(OperationContext* opCtx, const BSONObj& updateCmd);
UpdateCmdShape makeOneUpdateShape(OperationContext* opCtx, std::string_view updateCmd);
DeleteCmdShape makeOneDeleteShape(OperationContext* opCtx, const BSONObj& deleteCmd);
DeleteCmdShape makeOneDeleteShape(OperationContext* opCtx, std::string_view deleteCmd);

/**
 * An insert command has a single shape regardless of how many documents it inserts, so there is no
 * plural variant.
 */
InsertCmdShape makeInsertShape(std::string_view insertCmd);

/**
 * Builds the per-op request for the op at 'opIdx' of a bulkWrite command, the way the bulkWrite
 * execution path does (the command-level 'let' and the op's collation are applied).
 */
UpdateRequest makeBulkWriteUpdateRequest(OperationContext* opCtx,
                                         const BulkWriteCommandRequest& cmd,
                                         size_t opIdx);
DeleteRequest makeBulkWriteDeleteRequest(OperationContext* opCtx,
                                         const BulkWriteCommandRequest& cmd,
                                         size_t opIdx);

/**
 * Builds the shape of the op at 'opIdx' of a bulkWrite command.
 */
std::unique_ptr<BulkWriteUpdateCmdShape> makeBulkWriteUpdateShape(OperationContext* opCtx,
                                                                  std::string_view bulkCmd,
                                                                  size_t opIdx = 0);
std::unique_ptr<BulkWriteDeleteCmdShape> makeBulkWriteDeleteShape(OperationContext* opCtx,
                                                                  std::string_view bulkCmd,
                                                                  size_t opIdx = 0);
std::unique_ptr<BulkWriteInsertCmdShape> makeBulkWriteInsertShape(OperationContext* opCtx,
                                                                  std::string_view bulkCmd,
                                                                  size_t opIdx = 0);

}  // namespace mongo::query_shape

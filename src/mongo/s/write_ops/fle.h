// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/fle_crud.h"
#include "mongo/s/write_ops/bulk_write_reply_info.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Processes a response from an FLE insert/update/delete command and converts it to equivalent
 * BulkWriteReplyInfo.
 */
bulk_write_exec::BulkWriteReplyInfo processFLEResponse(OperationContext* opCtx,
                                                       const BatchedCommandRequest& request,
                                                       const BulkWriteCRUDOp::OpType& firstOpType,
                                                       bool errorsOnly,
                                                       const BatchedCommandResponse& response);

/**
 * Attempt to run the bulkWriteCommandRequest through Queryable Encryption code path.
 * Returns kNotProcessed if falling back to the regular bulk write code path is needed instead.
 *
 * This function may throw. Errors from FLE CRUD operations are reported via the function return;
 * errors from request validation or response processing propagate as exceptions.
 */
std::pair<FLEBatchResult, bulk_write_exec::BulkWriteReplyInfo> attemptExecuteFLE(
    OperationContext* opCtx, const BulkWriteCommandRequest& clientRequest);

}  // namespace mongo

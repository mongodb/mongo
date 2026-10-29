// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/bulk_write_reply_info.h"
#include "mongo/s/write_ops/unified_write_executor/stats.h"
#include "mongo/s/write_ops/write_command_ref.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace unified_write_executor {

struct FindAndModifyCommandResponse {
    // The reply of the findAndModify command.
    StatusWith<write_ops::FindAndModifyCommandReply> swReply;
    // The shardId where the findAndModify command is executed if known.
    boost::optional<ShardId> shardId;
    // The write concern error if exists.
    boost::optional<WriteConcernErrorDetail> wce;
};
using WriteCommandResponse = std::variant<BatchedCommandResponse,
                                          bulk_write_exec::BulkWriteReplyInfo,
                                          FindAndModifyCommandResponse>;

/**
 * This function will execute the specified write command and return a response.
 */
WriteCommandResponse executeWriteCommand(OperationContext* opCtx,
                                         WriteCommandRef cmdRef,
                                         unified_write_executor::Stats& stats,
                                         BSONObj originalCommand = BSONObj(),
                                         boost::optional<OID> targetEpoch = boost::none);

/**
 * Helper function for executing insert/update/delete commands.
 */
BatchedCommandResponse write(OperationContext* opCtx,
                             const BatchedCommandRequest& request,
                             unified_write_executor::Stats& stats,
                             boost::optional<OID> targetEpoch = boost::none);

/**
 * Helper function for executing bulk commands.
 */
bulk_write_exec::BulkWriteReplyInfo bulkWrite(OperationContext* opCtx,
                                              const BulkWriteCommandRequest& request,
                                              unified_write_executor::Stats& stats,
                                              BSONObj originalCommand = BSONObj());

/**
 * Helper function for executing findAndModify commands.
 */
FindAndModifyCommandResponse findAndModify(OperationContext* opCtx,
                                           const write_ops::FindAndModifyCommandRequest& request,
                                           BSONObj originalCommand = BSONObj());

/**
 * Unified write executor query knob check.
 */
bool isEnabled(OperationContext* opCtx);

}  // namespace unified_write_executor
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/s/write_ops/bulk_write_reply_info.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Constructs a BulkWriteCommandReply for the given 'replyInfo'. This function will also create a
 * cursor if needed.
 */
BulkWriteCommandReply populateCursorReply(OperationContext* opCtx,
                                          const BulkWriteCommandRequest& req,
                                          const BSONObj& reqObj,
                                          bulk_write_exec::BulkWriteReplyInfo replyInfo);

std::vector<BulkWriteReplyItem> exhaustCursorForReplyItems(
    OperationContext* opCtx, const ShardId& shardId, const BulkWriteCommandReply& commandReply);

}  // namespace mongo

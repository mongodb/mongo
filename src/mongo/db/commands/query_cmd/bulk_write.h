// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_parser.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <tuple>
#include <vector>

namespace mongo {
namespace bulk_write {

/**
 * Contains counters which aggregate all the individual bulk write responses.
 */
struct SummaryFields {
    int nErrors = 0;
    int nInserted = 0;
    int nMatched = 0;
    int nModified = 0;
    int nUpserted = 0;
    int nDeleted = 0;
};

using RetriedStmtIds = std::vector<int32_t>;
using BulkWriteReplyItems = std::vector<BulkWriteReplyItem>;
using BulkWriteReply = std::tuple<BulkWriteReplyItems, RetriedStmtIds, SummaryFields>;

BulkWriteReply performWrites(OperationContext* opCtx, const BulkWriteCommandRequest& req);

}  // namespace bulk_write
}  // namespace mongo

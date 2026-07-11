// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_parser.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace bulk_write_exec {

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

/**
 * Contains replies for individual bulk write ops along with the summary fields for all responses.
 */
struct BulkWriteReplyInfo {
    std::vector<BulkWriteReplyItem> replyItems;
    SummaryFields summaryFields;
    boost::optional<BulkWriteWriteConcernError> wcErrors;
    boost::optional<std::vector<StmtId>> retriedStmtIds;
};

}  // namespace bulk_write_exec
}  // namespace mongo

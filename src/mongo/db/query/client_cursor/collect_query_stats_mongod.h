// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Records certain metrics for the current operation on OpDebug and aggregates those metrics for
 * query stats use. If a cursor pin is provided, metrics are aggregated on the cursor; otherwise,
 * metrics are written directly to the query stats store.
 * NOTE: Metrics are taken from opDebug.additiveMetrics, so CurOp::setEndOfOpMetrics must be called
 * *prior* to calling these.
 */
void collectQueryStatsMongod(OperationContext* opCtx, ClientCursorPin& cursor);
// TODO SERVER-112083 Move this function to a better location since it has nothing to do with
// ClientCursor.
void collectQueryStatsMongod(OperationContext* opCtx, std::unique_ptr<query_stats::Key> key);

/**
 * Records query stats for reads and writes that error while the query stats key is still held by
 * the operation's OpDebug. It no-ops if no key was collected for this request.
 */
[[MONGO_MOD_PUBLIC]] void collectQueryStatsMongodErrored(OperationContext* opCtx,
                                                         ErrorCodes::Error errorCode);

/**
 * Marks the pinned cursor with 'ex', if it still has one, so that its query stats entry is written
 * as errored when the cursor is disposed. No query stats are written here. Must be called before
 * the cursor is deleted.
 */
void markCursorErroredForQueryStats(ClientCursorPin& cursorPin, const DBException& ex);

}  // namespace mongo

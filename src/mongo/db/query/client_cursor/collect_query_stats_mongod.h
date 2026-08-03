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
void collectQueryStatsMongod(OperationContext* opCtx,
                             const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             std::unique_ptr<query_stats::Key> key);

/**
 * Records query stats for reads that error while the query stats key is still held by the
 * operation's OpDebug. It no-ops for write commands, when no key was collected for this request, or
 * when the key was moved onto a cursor.
 */
[[MONGO_MOD_PUBLIC]] void collectQueryStatsMongodReadErrored(OperationContext* opCtx,
                                                             ErrorCodes::Error errorCode);

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Record metrics for the current operation on opDebug and aggregates those metrics for queryStats
 * use. If a cursor is provided (via ClusterClientCursorGuard or
 * ClusterCursorManager::PinnedCursor), metrics are aggregated on the cursor; otherwise, metrics are
 * written directly to the queryStats store.
 * NOTE: Metrics are taken from opDebug.additiveMetrics, so CurOp::setEndOfOpMetrics must be called
 * *prior* to calling these.
 */
void collectQueryStatsMongos(OperationContext* opCtx, std::unique_ptr<query_stats::Key> key);
void collectQueryStatsMongos(OperationContext* opCtx, ClusterClientCursorGuard& cursor);
void collectQueryStatsMongos(OperationContext* opCtx, ClusterCursorManager::PinnedCursor& cursor);

/**
 * Record metrics for a write operation. Writes are always batched into a single OperationContext on
 * the router, as it doesn't execute any writes on its own, but targets batches of writes for the
 * appropriate shards. We aggregate query stats metrics on the individual write up level, so it's
 * possible that we are collecting query stats for more than one such op.
 */
void collectQueryStatsMongosBatchWrites(OperationContext* opCtx);

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/client_cursor/collect_query_stats_mongod.h"

#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"

namespace mongo {

void collectQueryStatsMongod(OperationContext* opCtx, ClientCursorPin& pinnedCursor) {
    auto& opDebug = CurOp::get(opCtx)->debug();
    pinnedCursor->updateMetricsOnUnpin(opDebug.getAdditiveMetrics());
    pinnedCursor->updateMetricsOnUnpin(opDebug.changeStreamMetrics);

    // For a change stream query, we want to collect and update query stats on the initial query
    // and for every getMore.
    if (pinnedCursor->isChangeStreamQuery()) {
        auto snapshot = query_stats::captureMetrics(
            opCtx,
            query_stats::microsecondsToUint64(opDebug.getAdditiveMetrics().executionTime),
            opDebug.getAdditiveMetrics());

        query_stats::writeQueryStats(opCtx,
                                     opDebug.getQueryStatsInfo().keyHash,
                                     pinnedCursor->takeKey(),
                                     snapshot,
                                     {} /* supplementalMetrics */,
                                     pinnedCursor->isChangeStreamQuery());
    }
}

void collectQueryStatsMongod(OperationContext* opCtx,
                             const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             std::unique_ptr<query_stats::Key> key) {
    // If we haven't registered a cursor to prepare for getMore requests, we record
    // query stats directly.
    auto& opDebug = CurOp::get(opCtx)->debug();

    auto snapshot = query_stats::captureMetrics(
        opCtx,
        query_stats::microsecondsToUint64(opDebug.getAdditiveMetrics().executionTime),
        opDebug.getAdditiveMetrics());

    query_stats::writeQueryStats(opCtx,
                                 opDebug.getQueryStatsInfo().keyHash,
                                 std::move(key),
                                 snapshot,
                                 query_stats::computeSupplementalQueryStatsMetrics(opDebug));
}

}  // namespace mongo

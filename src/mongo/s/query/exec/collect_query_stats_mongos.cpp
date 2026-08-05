// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/collect_query_stats_mongos.h"

#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats


namespace mongo {

void collectQueryStatsMongos(OperationContext* opCtx, std::unique_ptr<query_stats::Key> key) {
    // If we haven't registered a cursor to prepare for getMore requests, we record
    // queryStats directly.
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

void collectQueryStatsMongos(OperationContext* opCtx, ClusterClientCursorGuard& cursor) {
    auto& opDebug = CurOp::get(opCtx)->debug();
    opDebug.getAdditiveMetrics().aggregateDataBearingNodeMetrics(cursor->takeRemoteMetrics());
    cursor->updateMetrics(opDebug.getAdditiveMetrics());
    cursor->updateMetrics(opDebug.changeStreamMetrics);

    // For a change stream query that never ends, we want to collect query stats on the initial
    // query and each getMore. Here we record the initial query.
    if (cursor->isChangeStreamCursor()) {
        auto snapshot = query_stats::captureMetrics(
            opCtx,
            query_stats::microsecondsToUint64(opDebug.getAdditiveMetrics().executionTime),
            opDebug.getAdditiveMetrics());

        query_stats::writeQueryStats(opCtx,
                                     opDebug.getQueryStatsInfo().keyHash,
                                     cursor->takeKey(),
                                     snapshot,
                                     {} /* supplementalMetrics */,
                                     cursor->isChangeStreamCursor());
    }
}

void collectQueryStatsMongos(OperationContext* opCtx, ClusterCursorManager::PinnedCursor& cursor) {
    auto& opDebug = CurOp::get(opCtx)->debug();
    opDebug.getAdditiveMetrics().aggregateDataBearingNodeMetrics(cursor->takeRemoteMetrics());
    cursor->updateMetrics(opDebug.getAdditiveMetrics());
    cursor->updateMetrics(opDebug.changeStreamMetrics);

    // For a change stream query that never ends, we want to update query stats for every getMore on
    // the cursor.
    if (cursor->isChangeStreamCursor()) {
        auto snapshot = query_stats::captureMetrics(
            opCtx,
            query_stats::microsecondsToUint64(opDebug.getAdditiveMetrics().executionTime),
            opDebug.getAdditiveMetrics());

        query_stats::writeQueryStats(opCtx,
                                     opDebug.getQueryStatsInfo().keyHash,
                                     nullptr,
                                     snapshot,
                                     {} /* supplementalMetrics */,
                                     cursor->isChangeStreamCursor());
    }
}

void collectQueryStatsMongosReadErrored(OperationContext* opCtx, ErrorCodes::Error errorCode) {
    if (!feature_flags::gFeatureFlagQueryStatsErrors.checkEnabled()) {
        return;
    }

    // Writes register their key per statement at a separate opIndex, never the slot read here, so
    // the lookup always misses for them. Batch writes additionally report per-statement failures in
    // 'writeErrors' under a top-level ok:1, where this hook is not called.
    auto& queryStatsInfo = CurOp::get(opCtx)->debug().getQueryStatsInfo();

    // Only record when there is a live key still owned by this operation. The key's lifetime on
    // OpDebug is therefore the window in which an error can be attributed to a shape. A null key
    // means:
    //  - The key was never created. registerRequest() opens the window, so an operation that fails
    //    ahead of it (command parsing, query shape computation) has no shape to attribute to and is
    //    invisible to $queryStats.
    //  - The key was moved into a cursor, which closes the window. ClusterClientCursorImpl's
    //    constructor std::moves it off OpDebug; mongos construction happens before the first batch
    //    is fetched.
    if (!queryStatsInfo.key) {
        return;
    }

    // We deliberately do not capture metrics here, as writeQueryStats/updateStatistics discards the
    // partial timing/exec metrics for errored snapshots.
    query_stats::QueryStatsSnapshot snapshot{};
    snapshot.errorCode = errorCode;

    query_stats::writeQueryStats(
        opCtx, queryStatsInfo.keyHash, std::move(queryStatsInfo.key), snapshot);
}

void collectQueryStatsMongosBatchWrites(OperationContext* opCtx) {
    auto& opDebug = CurOp::get(opCtx)->debug();
    opDebug.forEachQueryStatsInfoForBatchWrites([&](size_t opIndex, OpDebug::QueryStatsInfo& info) {
        auto snapshot = query_stats::captureMetrics(
            opCtx,
            query_stats::microsecondsToUint64(opDebug.getAdditiveMetrics(opIndex).executionTime),
            opDebug.getAdditiveMetrics(opIndex));
        query_stats::writeQueryStats(opCtx,
                                     opDebug.getQueryStatsInfo(opIndex).keyHash,
                                     std::move(opDebug.getQueryStatsInfo(opIndex).key),
                                     snapshot,
                                     query_stats::computeSupplementalQueryStatsMetrics(opDebug));
    });
}

}  // namespace mongo

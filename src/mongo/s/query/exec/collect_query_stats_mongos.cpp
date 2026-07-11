// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/collect_query_stats_mongos.h"

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

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/client_cursor/collect_query_stats_mongod.h"

#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"

namespace mongo {

void collectQueryStatsMongod(OperationContext* opCtx, ClientCursorPin& pinnedCursor) {
    auto& opDebug = CurOp::get(opCtx)->debug();
    pinnedCursor->updateMetricsOnUnpin(opDebug);
    pinnedCursor->captureSupplementalMetricsIfNeeded(opDebug);

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
                                     pinnedCursor->takeSupplementalMetrics(),
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

void collectQueryStatsMongodErrored(OperationContext* opCtx, ErrorCodes::Error errorCode) {
    if (!feature_flags::gFeatureFlagQueryStatsErrors.checkEnabled()) {
        return;
    }

    // Only read and write operations collect query stats errors. Skip other operations (e.g.,
    // commands that are neither reads nor writes).
    auto rwType = CurOp::get(opCtx)->getReadWriteType();
    if (rwType != Command::ReadWriteType::kRead && rwType != Command::ReadWriteType::kWrite) {
        return;
    }

    auto& opDebug = CurOp::get(opCtx)->debug();
    auto& queryStatsInfo = opDebug.getQueryStatsInfo();

    // Only record errors when there is a live key still owned by this operation. A null
    // key means we do not record error information for the following cases:
    //  - Errors that occur before the key was created (e.g., command parsing, query shape
    //  computation, etc.). We rely on registerRequest() to make a shape available to attribute the
    //  error to.
    //  - Errors that occur once the key was moved onto a cursor. Once the first batch is full on
    //  mongod, the ClientCursor constructor moves the key off of OpDebug (e.g., getMores) so a plan
    //  executor failure during that batch is still recorded here.
    if (!queryStatsInfo.key) {
        return;
    }

    // We deliberately do not call setEndOfOpMetrics here, as writeQueryStats/updateStatistics
    // discards the partial timing/exec metrics for errored snapshots.
    query_stats::QueryStatsSnapshot snapshot{};
    snapshot.errorCode = errorCode;

    query_stats::writeQueryStats(
        opCtx, queryStatsInfo.keyHash, std::move(queryStatsInfo.key), snapshot);
}

}  // namespace mongo

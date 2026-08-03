// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/client_cursor/collect_query_stats_mongod.h"

#include "mongo/db/query/query_feature_flags_gen.h"
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

void collectQueryStatsMongodReadErrored(OperationContext* opCtx, ErrorCodes::Error errorCode) {
    if (!feature_flags::gFeatureFlagQueryStatsErrors.checkEnabled()) {
        return;
    }

    // Writes register their key into the same OpDebug slot read below and nothing clears it on the
    // error path, so without this guard a failing write would be attributed an error code.
    // TODO SERVER-132417: Revisit when write command errors are supported.
    if (CurOp::get(opCtx)->getReadWriteType() != Command::ReadWriteType::kRead) {
        return;
    }

    auto& opDebug = CurOp::get(opCtx)->debug();
    auto& queryStatsInfo = opDebug.getQueryStatsInfo();

    // Only record errors when there is a live key still owned by this operation. A null
    // key means we do not record error information for:
    //  - Errors that occur before the key was created (eg. command parsing, query shape
    //  computation),
    //    since registerRequest() is what makes a shape available to attribute to.
    //  - Errors that occur once the key was moved onto a cursor. The ClientCursor constructor
    //  std::moves
    //    the key off OpDebug (eg. getMores), and on mongod that only happens after the first batch
    //    is full, so a plan executor failure during that batch is still recorded here.
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

/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
        query_stats::microsecondsToUint64(opDebug.additiveMetrics.executionTime),
        opDebug.additiveMetrics);

    query_stats::writeQueryStats(opCtx,
                                 opDebug.queryStatsInfo.keyHash,
                                 std::move(key),
                                 snapshot,
                                 query_stats::computeSupplementalQueryStatsMetrics(opDebug));
}

void collectQueryStatsMongos(OperationContext* opCtx, ClusterClientCursorGuard& cursor) {
    auto& opDebug = CurOp::get(opCtx)->debug();
    opDebug.additiveMetrics.aggregateDataBearingNodeMetrics(cursor->takeRemoteMetrics());
    cursor->incrementCursorMetrics(CurOp::get(opCtx)->debug().additiveMetrics);

    // For a change stream query that never ends, we want to collect query stats on the initial
    // query and each getMore. Here we record the initial query.
    if (cursor->getQueryStatsWillNeverExhaust()) {
        auto& opDebug = CurOp::get(opCtx)->debug();

        auto snapshot = query_stats::captureMetrics(
            opCtx,
            query_stats::microsecondsToUint64(opDebug.additiveMetrics.executionTime),
            opDebug.additiveMetrics);

        query_stats::writeQueryStats(opCtx,
                                     opDebug.queryStatsInfo.keyHash,
                                     cursor->takeKey(),
                                     snapshot,
                                     {} /* supplementalMetrics */,
                                     cursor->getQueryStatsWillNeverExhaust());
    }
}

void collectQueryStatsMongos(OperationContext* opCtx, ClusterCursorManager::PinnedCursor& cursor) {
    auto& opDebug = CurOp::get(opCtx)->debug();
    opDebug.additiveMetrics.aggregateDataBearingNodeMetrics(cursor->takeRemoteMetrics());
    cursor->incrementCursorMetrics(CurOp::get(opCtx)->debug().additiveMetrics);

    // For a change stream query that never ends, we want to update query stats for every getMore on
    // the cursor.
    if (cursor->getQueryStatsWillNeverExhaust()) {
        auto& opDebug = CurOp::get(opCtx)->debug();

        auto snapshot = query_stats::captureMetrics(
            opCtx,
            query_stats::microsecondsToUint64(opDebug.additiveMetrics.executionTime),
            opDebug.additiveMetrics);

        query_stats::writeQueryStats(opCtx,
                                     opDebug.queryStatsInfo.keyHash,
                                     nullptr,
                                     snapshot,
                                     {} /* supplementalMetrics */,
                                     cursor->getQueryStatsWillNeverExhaust());
    }
}

}  // namespace mongo

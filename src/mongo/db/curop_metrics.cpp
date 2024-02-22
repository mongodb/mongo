/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {
namespace {

// mongod only metrics
auto& deletedCounter = *MetricBuilder<Counter64>("document.deleted");
auto& insertedCounter = *MetricBuilder<Counter64>("document.inserted");
auto& returnedCounter = *MetricBuilder<Counter64>("document.returned");
auto& updatedCounter = *MetricBuilder<Counter64>("document.updated");

auto& scannedCounter = *MetricBuilder<Counter64>("queryExecutor.scanned");
auto& scannedObjectCounter = *MetricBuilder<Counter64>("queryExecutor.scannedObjects");

auto& scanAndOrderCounter = *MetricBuilder<Counter64>("operation.scanAndOrder");
auto& writeConflictsCounter = *MetricBuilder<Counter64>("operation.writeConflicts");

// mongos and mongod metrics
auto& killedDueToClientDisconnectCounter =
    *MetricBuilder<Counter64>("operation.killedDueToClientDisconnect");
auto& killedDueToMaxTimeMSExpiredCounter =
    *MetricBuilder<Counter64>("operation.killedDueToMaxTimeMSExpired");

}  // namespace

void recordCurOpMetrics(OperationContext* opCtx) {
    const OpDebug& debug = CurOp::get(opCtx)->debug();
    // TODO SERVER-78810 Use improved ClusterRole API here, and ensure we only add the non-router
    // metrics if `opCtx` didn't execute under the router-role.
    if (!serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        if (debug.additiveMetrics.nreturned)
            returnedCounter.increment(*debug.additiveMetrics.nreturned);
        if (debug.additiveMetrics.ninserted)
            insertedCounter.increment(*debug.additiveMetrics.ninserted);
        if (debug.additiveMetrics.nMatched)
            updatedCounter.increment(*debug.additiveMetrics.nMatched);
        if (debug.additiveMetrics.ndeleted)
            deletedCounter.increment(*debug.additiveMetrics.ndeleted);
        if (debug.additiveMetrics.keysExamined)
            scannedCounter.increment(*debug.additiveMetrics.keysExamined);
        if (debug.additiveMetrics.docsExamined)
            scannedObjectCounter.increment(*debug.additiveMetrics.docsExamined);

        if (debug.hasSortStage)
            scanAndOrderCounter.increment();
        if (auto n = debug.additiveMetrics.writeConflicts.load(); n > 0)
            writeConflictsCounter.increment(n);
        lookupPushdownCounters.incrementLookupCounters(CurOp::get(opCtx)->debug());
        sortCounters.incrementSortCounters(debug);
        queryFrameworkCounters.incrementQueryEngineCounters(CurOp::get(opCtx));
    }

    if (opCtx->getKillStatus() == ErrorCodes::ClientDisconnect) {
        killedDueToClientDisconnectCounter.increment();
    }

    if (opCtx->getKillStatus() == ErrorCodes::MaxTimeMSExpired ||
        debug.errInfo == ErrorCodes::MaxTimeMSExpired) {
        killedDueToMaxTimeMSExpiredCounter.increment();
    }
}

void recordCurOpMetricsOplogApplication(OperationContext* opCtx) {
    const OpDebug& debug = CurOp::get(opCtx)->debug();
    if (auto n = debug.additiveMetrics.writeConflicts.load(); n > 0) {
        writeConflictsCounter.increment(n);
    }
}

}  // namespace mongo

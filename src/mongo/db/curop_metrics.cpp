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

#include "mongo/platform/basic.h"

#include "mongo/base/counter.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/counters.h"

namespace mongo {
namespace {
Counter64 returnedCounter;
Counter64 insertedCounter;
Counter64 updatedCounter;
Counter64 deletedCounter;
Counter64 scannedCounter;
Counter64 scannedObjectCounter;

ServerStatusMetricField<Counter64> displayReturned("document.returned", &returnedCounter);
ServerStatusMetricField<Counter64> displayUpdated("document.updated", &updatedCounter);
ServerStatusMetricField<Counter64> displayInserted("document.inserted", &insertedCounter);
ServerStatusMetricField<Counter64> displayDeleted("document.deleted", &deletedCounter);
ServerStatusMetricField<Counter64> displayScanned("queryExecutor.scanned", &scannedCounter);
ServerStatusMetricField<Counter64> displayScannedObjects("queryExecutor.scannedObjects",
                                                         &scannedObjectCounter);

Counter64 scanAndOrderCounter;
Counter64 writeConflictsCounter;

ServerStatusMetricField<Counter64> displayScanAndOrder("operation.scanAndOrder",
                                                       &scanAndOrderCounter);
ServerStatusMetricField<Counter64> displayWriteConflicts("operation.writeConflicts",
                                                         &writeConflictsCounter);

}  // namespace

void recordCurOpMetrics(OperationContext* opCtx) {
    const OpDebug& debug = CurOp::get(opCtx)->debug();
    if (debug.nreturned > 0)
        returnedCounter.increment(debug.nreturned);
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

    queryEngineCounters.incrementQueryEngineCounters(CurOp::get(opCtx));
}

}  // namespace mongo

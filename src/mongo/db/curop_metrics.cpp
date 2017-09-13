/**
 *    Copyright (C) 2015 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
    if (debug.ninserted > 0)
        insertedCounter.increment(debug.ninserted);
    if (debug.nMatched > 0)
        updatedCounter.increment(debug.nMatched);
    if (debug.ndeleted > 0)
        deletedCounter.increment(debug.ndeleted);
    if (debug.keysExamined > 0)
        scannedCounter.increment(debug.keysExamined);
    if (debug.docsExamined > 0)
        scannedObjectCounter.increment(debug.docsExamined);

    if (debug.hasSortStage)
        scanAndOrderCounter.increment();
    if (debug.writeConflicts)
        writeConflictsCounter.increment(debug.writeConflicts);
}

}  // namespace mongo

/**
 * Tests that it is illegal to create a time-series collection within a transaction.
 * @tags: [
 *     uses_transactions,
 *     requires_fcv_49,
 * ]
 */
(function() {
"use strict";

if (!db.adminCommand({getParameter: 1, featureFlagTimeSeriesCollection: 1})
         .featureFlagTimeSeriesCollection.value) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const session = db.getMongo().startSession();
// Use a custom database, to avoid conflict with other tests that use the system.js collection.
session.startTransaction();
const testDB = session.getDatabase(jsTestName());
assert.commandFailedWithCode(testDB.createCollection('t', {timeseries: {timeField: 'time'}}),
                             ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
})();

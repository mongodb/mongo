/**
 * Tests that it is illegal to create a time-series collection within a transaction.
 * @tags: [
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

const session = db.getMongo().startSession();
// Use a custom database, to avoid conflict with other tests that use the system.js collection.
session.startTransaction();
const sessionDB = session.getDatabase('test');
assert.commandFailedWithCode(
    sessionDB.createCollection('timeseries_create_in_txn', {timeseries: {timeField: 'time'}}),
    ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
})();

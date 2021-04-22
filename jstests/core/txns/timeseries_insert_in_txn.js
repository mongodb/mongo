/**
 * Tests that it is illegal to insert documents into a time-series collection within a transaction.
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     uses_transactions,
 *     requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    // Use a custom database, to avoid conflict with other tests that use the system.js collection.
    const session = db.getMongo().startSession();
    const testDB = session.getDatabase(jsTestName());

    // Create time-series collection outside transaction.
    const collForCreate = testDB.getCollection('t');
    collForCreate.drop();
    const timeFieldName = 'time';
    assert.commandWorked(
        testDB.createCollection(collForCreate.getName(), {timeseries: {timeField: timeFieldName}}));

    session.startTransaction();
    const coll = testDB.getCollection(collForCreate.getName());
    assert.commandFailedWithCode(insert(coll, {_id: 0, [timeFieldName]: ISODate()}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
});
})();

// Tests that a change stream can be opened on a change collection when one exists, and that an
// exception is thrown if we attempt to open a stream while change streams are disabled.
// @tags: [
//   featureFlagMongoStore,
//   requires_fcv_61,
//   assumes_against_mongod_not_mongos,
// ]

(function() {
"use strict";

// TODO SERVER-66631 replace this with change stream disablement command. Extend the test cases for
// enablement/disablement combinations.
function disableChangeStream(connection) {
    const configDB = connection.getDB("config");
    assert(configDB.system.change_collection.drop());
}

(function runInReplicaSet() {
    const replSetTest = new ReplSetTest({nodes: 1});

    // TODO SERVER-67267 add 'featureFlagServerlessChangeStreams', 'multitenancySupport' and
    // 'serverless' flags and remove 'failpoint.forceEnableChangeCollectionsMode'.
    replSetTest.startSet(
        {setParameter: "failpoint.forceEnableChangeCollectionsMode=" + tojson({mode: "alwaysOn"})});

    replSetTest.initiate();

    const connection = replSetTest.getPrimary();

    // Insert a document to the 'stockPrice' collection.
    const testDb = connection.getDB("test");
    const csCursor = connection.getDB("test").stockPrice.watch([]);
    testDb.stockPrice.insert({_id: "mdb", price: 250});
    testDb.stockPrice.insert({_id: "tsla", price: 650});

    // Verify that the change stream observes the required event.
    assert.soon(() => csCursor.hasNext());
    const event1 = csCursor.next();
    assert.eq(event1.documentKey._id, "mdb");
    assert.soon(() => csCursor.hasNext());
    const event2 = csCursor.next();
    assert.eq(event2.documentKey._id, "tsla");

    // Disable the change stream while the change stream cursor is still opened.
    disableChangeStream(connection);

    // Verify that the cursor throws 'QueryPlanKilled' exception on doing get next.
    assert.throwsWithCode(() => assert.soon(() => csCursor.hasNext()), ErrorCodes.QueryPlanKilled);

    // Open a new change stream cursor with change stream disabled state and verify that
    // 'ChangeStreamNotEnabled' exception is thrown.
    assert.throwsWithCode(() => testDb.stock.watch([]), ErrorCodes.ChangeStreamNotEnabled);

    replSetTest.stopSet();
})();
}());

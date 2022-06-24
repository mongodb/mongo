// Tests that a change stream can be opened on a change collection when one exists, and that an
// exception is thrown if we attempt to open a stream while change streams are disabled.
// @tags: [
//   featureFlagServerlessChangeStreams,
//   multiversion_incompatible,
//   featureFlagMongoStore,
// ]

(function() {
"use strict";

// TODO SERVER-66632 replace this with change stream disablement command. Extend the test cases for
// enablement/disablement combinations.
function disableChangeStream(connection) {
    const configDB = connection.getDB("config");
    assert(configDB.system.change_collection.drop());
}

(function runInReplicaSet() {
    // TODO SERVER-66892 remove test-fixtures and let change stream passthrough create the test
    // environment.
    const replSetTest = new ReplSetTest({nodes: 1});
    replSetTest.startSet({setParameter: "multitenancySupport=true"});
    replSetTest.initiate();
    const connection = replSetTest.getPrimary();

    // Insert a document to the 'stockPrice' collection.
    const testDb = connection.getDB("test");
    const csCursor1 = connection.getDB("test").stockPrice.watch([]);
    testDb.stockPrice.insert({_id: "mdb", price: 250});

    // Verify that the change stream observes the required event.
    assert.soon(() => csCursor1.hasNext());
    const event = csCursor1.next();
    assert.eq(event.documentKey._id, "mdb");

    // Disable the change stream while the change stream cursor is still opened.
    disableChangeStream(connection);

    // Verify that the cursor throws 'QueryPlanKilled' exception on doing get next.
    assert.throwsWithCode(() => assert.soon(() => csCursor1.hasNext()), ErrorCodes.QueryPlanKilled);

    // Open a new change stream cursor with change stream disabled state and verify that
    // 'ChangeStreamNotEnabled' exception is thrown.
    assert.throwsWithCode(() => connection.getDB("test").stock.watch([]),
                          ErrorCodes.ChangeStreamNotEnabled);

    replSetTest.stopSet();
})();
}());

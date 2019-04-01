/**
 * Test that we can successfully commit a prepared transaction before the stable timestamp.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/aggregation/extras/utils.js");
    load("jstests/core/txns/libs/prepare_helpers.js");

    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();

    const dbName = "test";
    const collName = "commit_prepared_transaction_before_stable_timestamp";
    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    assert.commandWorked(testDB.runCommand({create: collName}));

    // Make sure there is no lag between the oldest timestamp and the stable timestamp so we can
    // test that committing a prepared transaction behind the oldest timestamp succeeds.
    assert.commandWorked(primary.adminCommand({
        "configureFailPoint": 'WTSetOldestTSToStableTS',
        "mode": 'alwaysOn',
    }));

    const session = primary.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 1}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    jsTestLog("Do a majority write to advance the stable timestamp past the prepareTimestamp");
    // Doing a majority write after preparing the transaction ensures that the stable timestamp is
    // past the prepare timestamp because this write must be in the committed snapshot.
    assert.commandWorked(
        testColl.runCommand("insert", {documents: [{_id: 2}]}, {writeConcern: {w: "majority"}}));

    jsTestLog("Committing the transaction before the stable timestamp");

    // Since we have advanced the stableTimestamp to be after the prepareTimestamp, when we commit
    // at the prepareTimestamp, we are certain that we are committing behind the stableTimestamp.
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    // Make sure we can see the insert from the prepared transaction.
    arrayEq(sessionColl.find().toArray(), [{_id: 1}, {_id: 2}]);

    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'WTSetOldestTSToStableTS', mode: 'off'}));

    replTest.stopSet();
}());
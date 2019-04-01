/**
 * Tests that prepared transactions can survive state transitions from SECONDARY to RECOVERING and
 * RECOVERING to SECONDARY.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const replSet = new ReplSetTest({nodes: 2});
    replSet.startSet();
    replSet.initiate();

    const primary = replSet.getPrimary();
    const secondary = replSet.getSecondary();

    const dbName = "test";
    const collName = "prepare_transaction_survives_state_transitions_to_and_from_recovering";
    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    assert.commandWorked(testDB.runCommand({create: collName}));
    assert.commandWorked(testColl.insert({_id: 1}));

    const session1 = primary.startSession({causalConsistency: false});
    const sessionDB1 = session1.getDatabase(dbName);
    const sessionColl1 = sessionDB1.getCollection(collName);

    const session2 = primary.startSession({causalConsistency: false});
    const sessionDB2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDB2.getCollection(collName);

    jsTestLog("Starting a transaction and putting it into prepare");

    session1.startTransaction();
    assert.commandWorked(sessionColl1.update({_id: 1}, {_id: 1, a: 1}));
    const prepareTimestamp1 = PrepareHelpers.prepareTransaction(session1);

    jsTestLog("Starting a second transaction and putting it into prepare");

    session2.startTransaction();
    assert.commandWorked(sessionColl2.insert({_id: 2}));
    const prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2);
    replSet.awaitReplication();

    jsTestLog("Putting secondary in maintenance mode so it will transition to RECOVERING");

    assert.commandWorked(secondary.adminCommand({replSetMaintenance: 1}));
    replSet.waitForState(secondary, ReplSetTest.State.RECOVERING);

    jsTestLog("Commiting the second prepared transaction while a node is in the RECOVERING state");

    assert.commandWorked(PrepareHelpers.commitTransaction(session2, prepareTimestamp2));
    replSet.awaitReplication();

    jsTestLog("Taking secondary out of maintenance mode so it will transition back to SECONDARY");

    assert.commandWorked(secondary.adminCommand({replSetMaintenance: 0}));
    replSet.waitForState(secondary, ReplSetTest.State.SECONDARY);

    jsTestLog("Stepping up the secondary");

    replSet.stepUp(secondary);
    replSet.waitForState(secondary, ReplSetTest.State.PRIMARY);
    const newPrimary = replSet.getPrimary();
    const newPrimaryDB = newPrimary.getDB(dbName);

    // Create a proxy session to reuse the session state of the old primary.
    const newSession = new _DelegatingDriverSession(newPrimary, session1);

    jsTestLog("Make sure that the transaction is properly prepared");

    // Make sure that we can't read changes to the document from the second transaction after
    // recovery.
    assert.eq(newPrimaryDB.getCollection(collName).find().toArray(), [{_id: 1}, {_id: 2}]);

    // Make sure that another write on the same document from the second transaction causes a write
    // conflict.
    assert.commandFailedWithCode(
        newPrimaryDB.runCommand(
            {update: collName, updates: [{q: {_id: 1}, u: {$set: {a: 1}}}], maxTimeMS: 5 * 1000}),
        ErrorCodes.MaxTimeMSExpired);

    // Make sure that we cannot add other operations to the second transaction since it is prepared.
    assert.commandFailedWithCode(
        newSession.getDatabase(dbName).getCollection(collName).insert({_id: 3}),
        ErrorCodes.PreparedTransactionInProgress);

    jsTestLog("Verify that the locks from the prepared transaction are still held");

    assert.commandFailedWithCode(newPrimaryDB.runCommand({drop: collName, maxTimeMS: 5 * 1000}),
                                 ErrorCodes.MaxTimeMSExpired);

    jsTestLog("Committing transaction");

    assert.commandWorked(PrepareHelpers.commitTransaction(newSession, prepareTimestamp1));
    replSet.awaitReplication();

    replSet.stopSet();

}());

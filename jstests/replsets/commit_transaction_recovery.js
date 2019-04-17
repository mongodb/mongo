/**
 * Test that startup recovery successfully applies all operations from a transaction after
 * replaying the commitTransaction oplog entry. We hold back the snapshot so that we make sure that
 * the operations from the transaction are not reflected in the data when recovery starts.
 *
 * @tags: [requires_persistence, uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    let primary = replTest.getPrimary();

    const dbName = "test";
    const collName = "commit_transaction_recovery";
    let testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    testDB.runCommand({drop: collName});
    assert.commandWorked(testDB.runCommand({create: collName}));

    let session = primary.startSession({causalConsistency: false});
    let sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 1}));
    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    jsTestLog("Disable snapshotting on all nodes");
    // Disable snapshotting so that future operations do not enter the majority snapshot.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "disableSnapshotting", mode: "alwaysOn"}));

    jsTestLog("Committing the transaction");
    // Since the commitTimestamp is after the last snapshot, this oplog entry will be replayed
    // during replication recovery during restart.
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    jsTestLog("Restarting node");

    // Perform a clean shutdown and restart. Note that the 'disableSnapshotting' failpoint will be
    // unset on the node following the restart.
    replTest.restart(primary);

    jsTestLog("Node was restarted");

    primary = replTest.getPrimary();
    testDB = primary.getDB(dbName);
    session = primary.startSession({causalConsistency: false});
    sessionDB = session.getDatabase(dbName);
    session.startTransaction();

    // Make sure that we can read the document from the transaction after recovery.
    assert.eq(testDB[collName].findOne({_id: 1}), {_id: 1});

    // Make sure that another write on the same document from the transaction has no write conflict.
    // Also, make sure that we can run another transaction after recovery without any problems.
    assert.commandWorked(sessionDB[collName].update({_id: 1}, {_id: 1, a: 1}));
    prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
    assert.eq(testDB[collName].findOne({_id: 1}), {_id: 1, a: 1});

    replTest.stopSet();
}());

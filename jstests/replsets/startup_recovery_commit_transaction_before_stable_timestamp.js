/**
 * Tests that the prepare oplog entry is applied as part of replaying commit oplog entry
 * during startup recovery.
 *
 * Also, tests that it does not re-apply the operations from the transaction if the data already
 * reflects the transaction. If the operations are replayed, this will cause a BSONTooLarge
 * exception.
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

    // Construct a large array such that two arrays in the same document are not greater than the
    // 16MB limit, but that three such arrays in the same document are greater than 16MB. This will
    // be helpful in recreating an idempotency issue that exists when applying the operations from
    // a transaction after the data already reflects the transaction.
    const largeArray = new Array(7 * 1024 * 1024).join('x');
    assert.commandWorked(testColl.insert([{_id: 1, "a": largeArray}]));

    // Start a transaction in a session that will be prepared and committed before node restart.
    let session = primary.startSession();
    let sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.update({_id: 1}, {$set: {"b": largeArray}}));
    assert.commandWorked(sessionColl.update({_id: 1}, {$unset: {"b": 1}}));
    assert.commandWorked(sessionColl.update({_id: 1}, {$set: {"c": largeArray}}));
    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    const recoveryTimestamp =
        assert.commandWorked(testColl.runCommand("insert", {documents: [{_id: 2}]})).operationTime;

    jsTestLog("Holding back the stable timestamp to right after the prepareTimestamp");

    // Hold back the stable timestamp to be right after the prepareTimestamp, but before the
    // commitTransaction oplog entry so that the transaction will be replayed during startup
    // recovery.
    assert.commandWorked(testDB.adminCommand({
        "configureFailPoint": 'holdStableTimestampAtSpecificTimestamp',
        "mode": 'alwaysOn',
        "data": {"timestamp": recoveryTimestamp}
    }));

    jsTestLog("Committing the transaction");

    // Since this transaction is committed after the last snapshot, this commit oplog entry will be
    // replayed during startup replication recovery.
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    jsTestLog("Restarting node");

    // Perform a clean shutdown and restart. And, the data restored at the storage recovery
    // timestamp should not reflect the transaction. If not, replaying the commit oplog entry during
    // startup recovery would throw BSONTooLarge exception.
    replTest.stop(primary, undefined, {skipValidation: true});
    // Since the oldest timestamp is same as the stable timestamp during node's restart, this test
    // will commit a transaction older than oldest timestamp during startup recovery.
    replTest.start(primary, {}, true);

    jsTestLog("Node was restarted");
    primary = replTest.getPrimary();

    // Make sure that the data reflects all the operations from the transaction after recovery.
    testDB = primary.getDB(dbName);
    const res = testDB[collName].findOne({_id: 1});
    assert.eq(res, {_id: 1, "a": largeArray, "c": largeArray});

    // Make sure that another write on the same document from the transaction has no write conflict.
    // Also, make sure that we can run another transaction after recovery without any problems.
    session = primary.startSession();
    sessionDB = session.getDatabase(dbName);
    session.startTransaction();
    assert.commandWorked(sessionDB[collName].update({_id: 1}, {_id: 1, a: 1}));
    prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
    assert.eq(testDB[collName].findOne({_id: 1}), {_id: 1, a: 1});

    replTest.stopSet();
}());

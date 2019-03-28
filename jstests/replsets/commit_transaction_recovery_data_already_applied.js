/**
 * Test that startup recovery successfully replays the commitTransaction oplog entry, but does not
 * re-apply the operations from the transaction if the data already reflects the transaction. If the
 * operations are replayed, this will cause a BSONTooLarge exception.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
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

    // Construct a large array such that two arrays in the same document are not greater than the
    // 16MB limit, but that three such arrays in the same document are greater than 16MB. This will
    // be helpful in recreating an idempotency issue that exists when applying the operations from
    // a transaction after the data already reflects the transaction.
    const largeArray = new Array(7 * 1024 * 1024).join('x');
    assert.commandWorked(testColl.insert({_id: 1, "a": largeArray}));

    session.startTransaction();
    assert.commandWorked(sessionColl.update({_id: 1}, {$set: {"b": largeArray}}));
    assert.commandWorked(sessionColl.update({_id: 1}, {$unset: {"b": 1}}));
    assert.commandWorked(sessionColl.update({_id: 1}, {$set: {"c": largeArray}}));
    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    const commitTimestamp =
        assert.commandWorked(testColl.runCommand("insert", {documents: [{_id: 2}]})).operationTime;
    const recoveryTimestamp =
        assert.commandWorked(testColl.runCommand("insert", {documents: [{_id: 3}]})).operationTime;

    jsTestLog("Holding back the stable timestamp to right after the commitTimestamp");

    // Hold back the stable timestamp to be right after the commitTimestamp, but before the
    // commitTransaction oplog entry so that the data will reflect the transaction during recovery.
    assert.commandWorked(testDB.adminCommand({
        "configureFailPoint": 'holdStableTimestampAtSpecificTimestamp',
        "mode": 'alwaysOn',
        "data": {"timestamp": recoveryTimestamp}
    }));

    jsTestLog("Committing the transaction");

    // Since the commitTimestamp is after the last snapshot, this oplog entry will be replayed
    // during replication recovery during restart.
    assert.commandWorked(PrepareHelpers.commitTransaction(session, commitTimestamp));

    jsTestLog("Restarting node");

    // Perform a clean shutdown and restart. Since the data already reflects the transaction, this
    // will error with BSONTooLarge only if recovery reapplies the operations from the transaction.
    // Note that the 'disableSnapshotting' failpoint will be unset on the node following the
    // restart.
    replTest.stop(primary, undefined, {skipValidation: true});
    // Restart primary with fail point "WTSetOldestTSToStableTS" to prevent lag between stable
    // timestamp and oldest timestamp during start up recovery period. We avoid lag to test if
    // we can prepare and commit a transaction older than oldest timestamp.
    // TODO: fail point "WTSetOldestTSToStableTS" has to be removed once SERVER-39870 is checked in.
    replTest.start(primary,
                   {setParameter: {'failpoint.WTSetOldestTSToStableTS': "{'mode': 'alwaysOn'}"}},
                   true);

    jsTestLog("Node was restarted");

    primary = replTest.getPrimary();
    testDB = primary.getDB(dbName);
    session = primary.startSession({causalConsistency: false});
    sessionDB = session.getDatabase(dbName);
    session.startTransaction();

    // Make sure that the data reflects all the operations from the transaction after recovery.
    const res = testDB[collName].findOne({_id: 1});
    assert.eq(res, {_id: 1, "a": largeArray, "c": largeArray}, tojson(res));

    // Make sure that another write on the same document from the transaction has no write conflict.
    // Also, make sure that we can run another transaction after recovery without any problems.
    assert.commandWorked(sessionDB[collName].update({_id: 1}, {_id: 1, a: 1}));
    prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(PrepareHelpers.commitTransactionAfterPrepareTS(session, prepareTimestamp));
    assert.eq(testDB[collName].findOne({_id: 1}), {_id: 1, a: 1});

    replTest.stopSet();
}());
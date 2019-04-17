/**
 * Test that we can successfully reconstruct a prepared transaction that was prepared before the
 * stable timestamp at the end of startup recovery.
 *
 * @tags: [requires_persistence, uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/aggregation/extras/utils.js");

    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    let primary = replTest.getPrimary();

    const dbName = "test";
    const collName = "startup_recovery_reconstructs_txn_prepared_before_stable";
    const testDB = primary.getDB(dbName);
    let testColl = testDB.getCollection(collName);

    assert.commandWorked(testColl.insert({_id: 0}));

    // Start a session on the primary.
    let session = primary.startSession();
    const sessionID = session.getSessionId();
    let sessionDB = session.getDatabase(dbName);
    let sessionColl = sessionDB.getCollection(collName);

    // Prepare the transaction on the session.
    session.startTransaction();
    // We are creating a record size of 14MB for _id '0', just to make sure when this
    // test runs with lesser wiredTiger cache size, there would be a higher possibility
    // of this record being considered for eviction from in-memory tree. And, to confirm
    // that we don't see problems like in SERVER-40422.
    const largeArray = new Array(14 * 1024 * 1024).join('x');
    assert.commandWorked(sessionColl.update({_id: 0}, {$set: {a: largeArray}}));
    assert.commandWorked(sessionColl.insert({_id: 1}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    // Fastcount reflects the insert of a prepared transaction.
    assert.eq(testColl.count(), 2);

    jsTestLog("Do a majority write to advance the stable timestamp past the prepareTimestamp");
    // Doing a majority write after preparing the transaction ensures that the stable timestamp is
    // past the prepare timestamp because this write must be in the committed snapshot.
    assert.commandWorked(
        testColl.runCommand("insert", {documents: [{_id: 2}]}, {writeConcern: {w: "majority"}}));

    // Fastcount reflects the insert of a prepared transaction.
    assert.eq(testColl.count(), 3);

    // Check that we have one transaction in the transactions table.
    assert.eq(primary.getDB('config')['transactions'].find().itcount(), 1);

    jsTestLog("Restarting node");
    // Perform a clean shutdown and restart. And, the data restored at the storage recovery
    // timestamp should not reflect the prepared transaction.
    replTest.stop(primary, undefined, {skipValidation: true});
    // Since the oldest timestamp is same as the stable timestamp during node's restart, this test
    // will reconstruct a prepared transaction older than oldest timestamp during startup recovery.
    replTest.start(primary, {}, true);

    jsTestLog("Node was restarted");
    primary = replTest.getPrimary();
    testColl = primary.getDB(dbName)[collName];

    // Make sure we cannot see the writes from the prepared transaction yet.
    arrayEq(testColl.find().toArray(), [{_id: 0}, {_id: 2}]);
    assert.eq(testColl.count(), 3);

    // Make sure there is still one transaction in the transactions table. This is because the
    // entry in the transactions table is made durable when a transaction is prepared.
    assert.eq(primary.getDB('config')['transactions'].find().itcount(), 1);

    // Make sure we can successfully commit the recovered prepared transaction.
    session = PrepareHelpers.createSessionWithGivenId(primary, sessionID);
    sessionDB = session.getDatabase(dbName);
    // The transaction on this session should have a txnNumber of 0. We explicitly set this
    // since createSessionWithGivenId does not restore the current txnNumber in the shell.
    session.setTxnNumber_forTesting(0);
    const txnNumber = session.getTxnNumber_forTesting();

    // Make sure we cannot add any operations to a prepared transaction.
    assert.commandFailedWithCode(sessionDB.runCommand({
        insert: collName,
        txnNumber: NumberLong(txnNumber),
        documents: [{_id: 10}],
        autocommit: false,
    }),
                                 ErrorCodes.PreparedTransactionInProgress);

    // Make sure that writing to a document that was updated in the prepared transaction causes
    // a write conflict.
    assert.commandFailedWithCode(
        sessionDB.runCommand(
            {update: collName, updates: [{q: {_id: 0}, u: {$set: {a: 2}}}], maxTimeMS: 5 * 1000}),
        ErrorCodes.MaxTimeMSExpired);

    jsTestLog("Committing the prepared transaction");
    assert.commandWorked(sessionDB.adminCommand({
        commitTransaction: 1,
        commitTimestamp: prepareTimestamp,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }));

    // Make sure we can see the effects of the prepared transaction.
    arrayEq(testColl.find().toArray(), [{_id: 0, a: largeArray}, {_id: 1}, {_id: 2}]);
    assert.eq(testColl.count(), 3);

    replTest.stopSet();
}());

/**
 * When the oplog size grows during initial sync to exceed the configured maximum, the node must
 * truncate the oplog only up to the oldest active transaction timestamp at the time of the last
 * stable checkpoint. The first oplog entry that belongs to an active transaction is preserved, and
 * all entries after it.
 *
 * This tests the oldestActiveTransactionTimestamp, which is calculated from the "startOpTime"
 * field of documents in the config.transactions collection.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

// A new replica set for both the commit and abort tests to ensure the same clean state.
function doTest(commitOrAbort) {
    const replSet = new ReplSetTest({
        oplogSize: PrepareHelpers.oplogSizeMB,
        // Oplog can be truncated each "sync" cycle. Increase its frequency to once per second.
        nodeOptions: {syncdelay: 1, setParameter: {logComponentVerbosity: tojson({storage: 1})}},
        nodes: 1
    });

    replSet.startSet(PrepareHelpers.replSetStartSetOptions);
    replSet.initiate();
    const primary = replSet.getPrimary();
    const primaryOplog = primary.getDB("local").oplog.rs;
    assert.lte(primaryOplog.dataSize(), PrepareHelpers.oplogSizeBytes);

    const coll = primary.getDB("test").test;
    assert.commandWorked(coll.insert({}, {writeConcern: {w: "majority"}}));

    jsTestLog("Prepare a transaction");

    const session = primary.startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("test").test.insert({myTransaction: 1}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    const txnEntry = primary.getDB("config").transactions.findOne();

    const oldestRequiredTimestampForCrashRecovery =
        PrepareHelpers.getOldestRequiredTimestampForCrashRecovery(primary.getDB("test"));
    assert.lte(oldestRequiredTimestampForCrashRecovery, prepareTimestamp);

    // Make sure that the timestamp of the first oplog entry for this transaction matches the
    // start timestamp in the transactions table.
    let oplog = primary.getDB("local").getCollection("oplog.rs");
    const txnNum = session.getTxnNumber_forTesting();
    const op = oplog.findOne({"txnNumber": txnNum, "lsid.id": session.getSessionId().id});
    assert.neq(op, null);
    const firstTxnOpTs = op.ts;
    assert.eq(txnEntry.startOpTime.ts, firstTxnOpTs, tojson(txnEntry));

    jsTestLog("Insert documents until oplog exceeds oplogSize");

    // Oplog with prepared txn grows indefinitely - let it reach twice its supposed max size.
    PrepareHelpers.growOplogPastMaxSize(replSet);

    jsTestLog("Make sure the transaction's first entry is still in the oplog");

    assert.eq(primaryOplog.find({ts: firstTxnOpTs}).itcount(), 1);

    jsTestLog("Add a secondary node");

    const secondary = replSet.add({rsConfig: {votes: 0, priority: 0}});
    replSet.reInitiate();

    jsTestLog("Reinitiated, awaiting secondary node");

    replSet.awaitSecondaryNodes();

    jsTestLog("Checking secondary oplog and config.transactions");

    // Oplog grew past maxSize, and it includes the oldest active transaction's entry.
    const secondaryOplog = secondary.getDB("local").oplog.rs;
    assert.gt(secondaryOplog.dataSize(), PrepareHelpers.oplogSizeBytes);
    assert.eq(secondaryOplog.find({ts: firstTxnOpTs}).itcount(), 1);

    const secondaryTxnEntry = secondary.getDB("config").transactions.findOne();
    assert.eq(secondaryTxnEntry, txnEntry, tojson(secondaryTxnEntry));

    if (commitOrAbort === "commit") {
        jsTestLog("Commit prepared transaction and wait for oplog to shrink to max oplogSize");
        assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
    } else if (commitOrAbort === "abort") {
        jsTestLog("Abort prepared transaction and wait for oplog to shrink to max oplogSize");
        assert.commandWorked(session.abortTransaction_forTesting());
    } else {
        throw new Error(`Unrecognized value for commitOrAbort: ${commitOrAbort}`);
    }

    replSet.awaitReplication();

    PrepareHelpers.awaitOplogTruncation(replSet);
    replSet.stopSet();
}
doTest("commit");
doTest("abort");
})();

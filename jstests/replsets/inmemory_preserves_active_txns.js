/**
 * When a primary's oplog size exceeds the configured maximum, it must truncate the oplog only up to
 * the oldest active transaction timestamp at the time of the last stable checkpoint. The first
 * oplog entry that belongs to an active transaction is preserved, and all entries after it. A
 * secondary with the inMemory storage engine must treat its own oplog the same.
 *
 * This tests the oldestActiveTransactionTimestamp, which is calculated from the "startOpTime"
 * field of documents in the config.transactions collection.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    // If the test runner passed --storageEngine=inMemory then we know inMemory is compiled into the
    // server. We'll actually use both inMemory and wiredTiger storage engines.
    const storageEngine = jsTest.options().storageEngine;
    if (storageEngine !== 'inMemory') {
        jsTestLog(`Skip test: storageEngine == "${storageEngine}", not "inMemory"`);
        return;
    }

    // A new replica set for both the commit and abort tests to ensure the same clean state.
    function doTest(commitOrAbort) {
        const replSet = new ReplSetTest({
            // Oplog can be truncated each "sync" cycle. Increase its frequency to once per second.
            nodeOptions:
                {syncdelay: 1, setParameter: {logComponentVerbosity: tojson({storage: 1})}},
            nodes: [
                {storageEngine: "wiredTiger"},
                // inMemory node must not be a voter, otherwise lastCommitted never advances
                {storageEngine: "inMemory", rsConfig: {priority: 0, votes: 0}},
            ],
            waitForKeys: false
        });

        replSet.startSet(PrepareHelpers.replSetStartSetOptions);
        replSet.initiateWithAnyNodeAsPrimary(
            null, "replSetInitiate", {doNotWaitForStableRecoveryTimestamp: true});

        const primary = replSet.getPrimary();
        const secondary = replSet.getSecondary();
        const primaryOplog = primary.getDB("local").oplog.rs;
        assert.lte(primaryOplog.dataSize(), PrepareHelpers.oplogSizeBytes);
        const secondaryOplog = secondary.getDB("local").oplog.rs;
        assert.lte(secondaryOplog.dataSize(), PrepareHelpers.oplogSizeBytes);

        const coll = primary.getDB("test").test;
        assert.commandWorked(coll.insert({}));

        jsTestLog("Prepare a transaction");

        const session = primary.startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("test").test.insert({myTransaction: 1}));
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

        const oldestRequiredTimestampForCrashRecovery =
            PrepareHelpers.getOldestRequiredTimestampForCrashRecovery(primary.getDB("test"));
        assert.lte(oldestRequiredTimestampForCrashRecovery, prepareTimestamp);

        jsTestLog("Get transaction entry from config.transactions");

        const txnEntry = primary.getDB("config").transactions.findOne();
        // The prepare oplog entry may or may not be the first oplog entry depending on packing.
        assert.lte(txnEntry.startOpTime.ts, prepareTimestamp, tojson(txnEntry));

        assert.soonNoExcept(() => {
            const secondaryTxnEntry = secondary.getDB("config").transactions.findOne();
            assert(secondaryTxnEntry);
            assert.eq(secondaryTxnEntry, txnEntry, tojson(secondaryTxnEntry));
            return true;
        });

        jsTestLog("Find prepare oplog entry");

        const oplogEntry = PrepareHelpers.findPrepareEntry(primaryOplog);
        assert.eq(oplogEntry.ts, prepareTimestamp, tojson(oplogEntry));
        // Must already be written on secondary, since the config.transactions entry is.
        const secondaryOplogEntry = PrepareHelpers.findPrepareEntry(secondaryOplog);
        assert.eq(secondaryOplogEntry.ts, prepareTimestamp, tojson(secondaryOplogEntry));

        jsTestLog("Insert documents until oplog exceeds oplogSize");

        // Oplog with prepared txn grows indefinitely - let it reach twice its supposed max size.
        PrepareHelpers.growOplogPastMaxSize(replSet);

        jsTestLog(
            `Oplog dataSize = ${primaryOplog.dataSize()}, check the prepare entry still exists`);

        assert.eq(oplogEntry, PrepareHelpers.findPrepareEntry(primaryOplog));
        assert.soon(() => {
            return secondaryOplog.dataSize() > PrepareHelpers.oplogSizeBytes;
        });
        assert.eq(oplogEntry, PrepareHelpers.findPrepareEntry(secondaryOplog));

        if (commitOrAbort === "commit") {
            jsTestLog("Commit prepared transaction and wait for oplog to shrink to max oplogSize");
            PrepareHelpers.commitTransaction(session, prepareTimestamp);
        } else if (commitOrAbort === "abort") {
            jsTestLog("Abort prepared transaction and wait for oplog to shrink to max oplogSize");
            assert.commandWorked(session.abortTransaction_forTesting());
        } else {
            throw new Error(`Unrecognized value for commitOrAbort: ${commitOrAbort}`);
        }

        PrepareHelpers.awaitOplogTruncation(replSet);

        replSet.stopSet();
    }

    doTest("commit");
    doTest("abort");
})();

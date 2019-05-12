/**
 * When a primary's oplog size exceeds the configured maximum, it must truncate the oplog only up to
 * the oldest active transaction timestamp at the time of the last stable checkpoint. The first
 * oplog entry that belongs to an active transaction is preserved, and all entries after it.
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
            // Oplog can be truncated each "sync" cycle. Increase its frequency to once per second.
            nodeOptions:
                {syncdelay: 1, setParameter: {logComponentVerbosity: tojson({storage: 1})}},
            nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]
        });

        replSet.startSet(PrepareHelpers.replSetStartSetOptions);
        replSet.initiate();

        const primary = replSet.getPrimary();
        const secondary = replSet.getSecondary();
        const primaryOplog = primary.getDB("local").oplog.rs;
        assert.lte(primaryOplog.dataSize(), PrepareHelpers.oplogSizeBytes);
        const secondaryOplog = secondary.getDB("local").oplog.rs;
        assert.lte(secondaryOplog.dataSize(), PrepareHelpers.oplogSizeBytes);

        const coll = primary.getDB("test").test;
        assert.commandWorked(coll.insert({}, {writeConcern: {w: "majority"}}));

        jsTestLog("Prepare a transaction");

        const session = primary.startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("test").test.insert({myTransaction: 1}));
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

        jsTestLog("Get transaction entry from config.transactions");

        const txnEntry = primary.getDB("config").transactions.findOne();
        if (TestData.setParameters.useMultipleOplogEntryFormatForTransactions) {
            assert.lte(txnEntry.startOpTime.ts, prepareTimestamp, tojson(txnEntry));
        } else {
            assert.eq(txnEntry.startOpTime.ts, prepareTimestamp, tojson(txnEntry));
        }

        assert.soonNoExcept(() => {
            const secondaryTxnEntry = secondary.getDB("config").transactions.findOne();
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
            session.abortTransaction_forTesting();
        } else {
            throw new Error(`Unrecognized value for commitOrAbort: ${commitOrAbort}`);
        }

        PrepareHelpers.awaitOplogTruncation(replSet);

        replSet.stopSet();
    }

    doTest("commit");
    doTest("abort");
})();

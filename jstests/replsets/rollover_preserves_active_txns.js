/**
 * When a primary's oplog size exceeds the configured maximum, it must truncate the oplog only up to
 * the oldest active transaction timestamp at the time of the last stable checkpoint. The first
 * oplog entry that belongs to a prepared uncommitted transaction is preserved, and all entries
 * after it.
 *
 * This tests the oldestActiveTransactionTimestamp, which is calculated from the "startTimestamp"
 * field of documents in the config.transactions collection.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const oplogSizeMB = 1;
    const oplogSizeBytes = oplogSizeMB * 1024 * 1024;
    const tenKB = new Array(10 * 1024).join("a");

    // A new replica set for both the commit and abort tests to ensure the same clean state.
    function doTest(commitOrAbort) {
        const replSet = new ReplSetTest({
            // Oplog can be truncated each "sync" cycle. Increase its frequency to once per second.
            nodeOptions: {syncdelay: 1},
            nodes: 2
        });

        replSet.startSet({oplogSize: oplogSizeMB});
        replSet.initiate();

        const primary = replSet.getPrimary();
        const secondary = replSet.getSecondary();
        const primaryOplog = primary.getDB("local").oplog.rs;
        assert.lte(primaryOplog.dataSize(), oplogSizeBytes);
        const secondaryOplog = secondary.getDB("local").oplog.rs;
        assert.lte(secondaryOplog.dataSize(), oplogSizeBytes);

        const coll = primary.getDB("test").test;
        assert.commandWorked(coll.insert({}, {writeConcern: {w: "majority"}}));

        jsTestLog("Prepare a transaction");

        const session = primary.startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("test").test.insert({myTransaction: 1}));
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

        jsTestLog("Get transaction entry from config.transactions");

        const txnEntry = primary.getDB("config").transactions.findOne();
        assert.eq(txnEntry.startOpTime.ts, prepareTimestamp, tojson(txnEntry));

        assert.soonNoExcept(() => {
            const secondaryTxnEntry = secondary.getDB("config").transactions.findOne();
            assert.eq(secondaryTxnEntry, txnEntry, tojson(secondaryTxnEntry));
            return true;
        });

        jsTestLog("Find prepare oplog entry");

        const oplogEntry = primaryOplog.findOne({prepare: true});
        assert.eq(oplogEntry.ts, prepareTimestamp, tojson(oplogEntry));
        // Must already be written on secondary, since the config.transactions entry is.
        const secondaryOplogEntry = secondaryOplog.findOne({prepare: true});
        assert.eq(secondaryOplogEntry.ts, prepareTimestamp, tojson(secondaryOplogEntry));

        jsTestLog("Insert documents until oplog exceeds oplogSize");

        // Oplog with prepared txn grows indefinitely - let it reach twice its supposed max size.
        while (primaryOplog.dataSize() <= 2 * oplogSizeBytes) {
            assert.commandWorked(coll.insert({tenKB: tenKB}));
        }

        jsTestLog(
            `Oplog dataSize = ${primaryOplog.dataSize()}, check the prepare entry still exists`);

        assert.eq(oplogEntry, primaryOplog.findOne({prepare: true}));
        assert.soon(() => {
            return secondaryOplog.dataSize() > oplogSizeBytes;
        });
        assert.eq(oplogEntry, secondaryOplog.findOne({prepare: true}));

        if (commitOrAbort === "commit") {
            jsTestLog("Commit prepared transaction and wait for oplog to shrink to max oplogSize");
            PrepareHelpers.commitTransaction(session, prepareTimestamp);
        } else if (commitOrAbort === "abort") {
            jsTestLog("Abort prepared transaction and wait for oplog to shrink to max oplogSize");
            session.abortTransaction_forTesting();
        } else {
            throw new Error(`Unrecognized value for commitOrAbort: ${commitOrAbort}`);
        }

        jsTestLog("Add writes after transaction finished to trigger oplog reclamation");

        // Old entries are reclaimed when oplog size reaches new milestone. With a 1MB oplog,
        // milestones are every 0.1 MB (see WiredTigerRecordStore::OplogStones::OplogStones) so
        // write about 0.2 MB to be certain.
        for (var i = 0; i < 200; i++) {
            assert.commandWorked(coll.insert({tenKB: tenKB}));
        }

        jsTestLog("Waiting for oplog to shrink to 1MB");

        for (let [nodeName, oplog] of[["primary", primaryOplog], ["secondary", secondaryOplog]]) {
            assert.soon(function() {
                const dataSize = oplog.dataSize();
                const prepareEntryRemoved = (oplog.findOne({prepare: true}) === null);
                print(
                    `${nodeName} oplog dataSize: ${dataSize}, prepare entry removed: ${prepareEntryRemoved}`);
                // The oplog milestone system allows the oplog to grow to 110% its max size.
                if (dataSize < 1.1 * oplogSizeBytes && prepareEntryRemoved) {
                    return true;
                }

                assert.commandWorked(coll.insert({tenKB: tenKB}, {writeConcern: {w: "majority"}}));
                return false;
            }, `waiting for ${nodeName} oplog reclamation`, ReplSetTest.kDefaultTimeoutMS, 1000);
        }

        replSet.stopSet();
    }

    doTest("commit");
    doTest("abort");
})();

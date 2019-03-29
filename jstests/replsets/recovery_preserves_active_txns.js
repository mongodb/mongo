/**
 * When the oplog size grows during recovery to exceed the configured maximum, the node must
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
    load("jstests/libs/check_log.js");

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
        const primaryOplog = primary.getDB("local").oplog.rs;
        assert.lte(primaryOplog.dataSize(), PrepareHelpers.oplogSizeBytes);

        const coll = primary.getDB("test").test;
        assert.commandWorked(coll.insert({}, {writeConcern: {w: "majority"}}));

        jsTestLog("Prepare a transaction");

        const session = primary.startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("test").test.insert({myTransaction: 1}));
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

        jsTestLog("Insert documents until oplog exceeds oplogSize");

        // Oplog with prepared txn grows indefinitely - let it reach twice its supposed max size.
        PrepareHelpers.growOplogPastMaxSize(replSet);

        // Oplog grew past maxSize, and it includes the oldest active transaction's entry.
        var secondary = replSet.getSecondary();
        function checkSecondaryOplog() {
            const secondaryOplog = secondary.getDB("local").oplog.rs;
            assert.soon(() => {
                return secondaryOplog.dataSize() >= PrepareHelpers.oplogSizeBytes;
            }, "waiting for secondary oplog to grow", ReplSetTest.kDefaultTimeoutMS);
            const secondaryOplogEntry = secondaryOplog.findOne({prepare: true});
            assert.eq(secondaryOplogEntry.ts, prepareTimestamp, tojson(secondaryOplogEntry));
        }
        checkSecondaryOplog();

        jsTestLog("Restart the secondary");

        const secondaryId = replSet.getSecondary().nodeId;
        // Validation can't complete while the active transaction holds a lock.
        replSet.stop(secondaryId, undefined, {skipValidation: true});
        secondary = replSet.start(secondaryId, {}, true /* restart */);

        jsTestLog("Restarted");

        replSet.waitForState(replSet.getSecondaries(), ReplSetTest.State.SECONDARY);
        checkSecondaryOplog();

        if (commitOrAbort === "commit") {
            jsTestLog("Commit prepared transaction and wait for oplog to shrink to max oplogSize");
            PrepareHelpers.commitTransactionAfterPrepareTS(session, prepareTimestamp);
        } else if (commitOrAbort === "abort") {
            jsTestLog("Abort prepared transaction and wait for oplog to shrink to max oplogSize");
            session.abortTransaction_forTesting();
        } else {
            throw new Error(`Unrecognized value for commitOrAbort: ${commitOrAbort}`);
        }

        PrepareHelpers.awaitOplogTruncation(replSet);

        // ReplSetTest reacts poorly to restarting a node, end it manually.
        replSet.stopSet(true, false, {});
    }
    doTest("commit");
    doTest("abort");
})();

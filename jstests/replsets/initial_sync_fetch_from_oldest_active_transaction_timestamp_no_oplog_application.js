/**
 * Tests that initial sync properly fetches from the oldest active transaction timestamp, even if
 * the beginApplyingTimestamp and the stopTimestamp are the same. The beginApplyingTimestamp is the
 * timestamp of the oplog entry that was last applied on the sync source before initial sync begins.
 * It is also the timestamp after which we will start applying operations during initial sync. The
 * stopTimestamp is the timestamp of the oplog entry that was last applied on the sync source when
 * before the oplog application phase of initial sync begins. If they are the same, it means that
 * no operations were run on the sync source during the collection cloning phase of initial sync and
 * so no oplog entries need to be applied.
 *
 * To make sure that it is applying from the correct point, the test prepares a transaction before
 * the beginFetchingTimestamp and commits it before the beginApplyingTimestamp. Since the
 * transaction is not active by the time initial sync begins, its prepareTransaction oplog entry
 * won't be fetched during oplog application and trying to apply the commitTransaction oplog entry
 * will cause initial sync to fail.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";

    load("jstests/libs/check_log.js");
    load("jstests/core/txns/libs/prepare_helpers.js");

    const replTest = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();
    let secondary = replTest.getSecondary();

    const dbName = "test";
    const collName =
        "initial_sync_fetch_from_oldest_active_transaction_timestamp_no_oplog_application";
    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    assert.commandWorked(testColl.insert({_id: 1}));

    jsTestLog("Preparing a transaction that will later be committed");

    const session1 = primary.startSession({causalConsistency: false});
    const sessionDB1 = session1.getDatabase(dbName);
    const sessionColl1 = sessionDB1.getCollection(collName);
    session1.startTransaction();
    assert.commandWorked(sessionColl1.insert({_id: 2}));
    const prepareTimestamp1 = PrepareHelpers.prepareTransaction(session1);

    jsTestLog("Preparing a transaction that will be the oldest active transaction");

    // Prepare a transaction so that there is an active transaction with an oplog entry. The
    // timestamp of the first oplog entry of this transaction will become the beginFetchingTimestamp
    // during initial sync.
    const session2 = primary.startSession({causalConsistency: false});
    const sessionDB2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDB2.getCollection(collName);
    session2.startTransaction();
    assert.commandWorked(sessionColl2.insert({_id: 3}));
    const prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2);

    const oplog = primary.getDB("local").getCollection("oplog.rs");
    const txnNum = session2.getTxnNumber_forTesting();
    const op = oplog.findOne({"txnNumber": txnNum, "lsid.id": session2.getSessionId().id});
    assert.neq(op, null);
    const beginFetchingTs = op.ts;
    jsTestLog("Expected beginFetchingTimestamp: " + beginFetchingTs);

    // Commit the first transaction so that we have an operation that is fetched during initial sync
    // but should not be applied. If this is applied, initial sync will fail because while trying to
    // apply the commitTransaction oplog entry, it will fail to get the prepareTransaction oplog
    // entry since its optime is before the beginFetchingTimestamp. Doing another operation will
    // also cause the beginApplyingTimestamp to be different from the beginFetchingTimestamp. Note
    // that since the beginApplyingTimestamp is the timestamp after which operations are applied
    // during initial sync, this commitTransaction will not be applied.
    const beginApplyingTimestamp =
        assert.commandWorked(PrepareHelpers.commitTransaction(session1, prepareTimestamp1))
            .operationTime;

    jsTestLog("beginApplyingTimestamp/stopTimestamp: " + beginApplyingTimestamp);

    // Restart the secondary with startClean set to true so that it goes through initial sync. Since
    // we won't be running any operations during collection cloning, the beginApplyingTimestamp and
    // stopTimestamp should be the same.
    replTest.stop(secondary,
                  // signal
                  undefined,
                  // Validation would encounter a prepare conflict on the open transaction.
                  {skipValidation: true});
    secondary = replTest.start(secondary,
                               {startClean: true, setParameter: {'numInitialSyncAttempts': 1}},
                               true /* wait */);
    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();

    jsTestLog("Secondary was restarted");

    // Wait for the secondary to complete initial sync.
    replTest.waitForState(secondary, ReplSetTest.State.SECONDARY);

    jsTestLog("Initial sync completed");

    // Make sure the secondary fetched enough transaction oplog entries.
    secondary.setSlaveOk();
    const secondaryOplog = secondary.getDB("local").getCollection("oplog.rs");
    assert.eq(secondaryOplog.find({"ts": beginFetchingTs}).itcount(), 1);

    // Make sure the first transaction committed properly and is reflected after the initial sync.
    let res = secondary.getDB(dbName).getCollection(collName).findOne({_id: 2});
    assert.docEq(res, {_id: 2}, res);

    jsTestLog("Aborting the second transaction");

    session2.abortTransaction_forTesting();

    replTest.stopSet();
})();

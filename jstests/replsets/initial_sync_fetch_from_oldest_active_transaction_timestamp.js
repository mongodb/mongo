/**
 * Tests that initial sync properly fetches from the oldest active transaction timestamp, but that
 * it only applies from the beginApplyingTimestamp. The beginApplyingTimestamp is the timestamp of
 * the oplog entry that was last applied on the sync source before initial sync begins. It is also
 * the timestamp after which we will start applying operations during initial sync.
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
    const collName = "initial_sync_fetch_from_oldest_active_transaction_timestamp";
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

    jsTestLog("Preparing a transaction that will later be the oldest active transaction");

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

    jsTestLog("beginApplyingTimestamp: " + beginApplyingTimestamp);

    // Restart the secondary with startClean set to true so that it goes through initial sync. Also
    // restart the node with a failpoint turned on that will pause initial sync after the secondary
    // has copied {_id: 1} and {_id: 2}. This way we can insert more documents when initial sync is
    // paused and know that they won't be copied during collection cloning but instead must be
    // applied during oplog application.
    replTest.stop(secondary,
                  // signal
                  undefined,
                  // Validation would encounter a prepare conflict on the open transaction.
                  {skipValidation: true});
    secondary = replTest.start(
        secondary,
        {
          startClean: true,
          setParameter: {
              'failpoint.initialSyncHangDuringCollectionClone': tojson(
                  {mode: 'alwaysOn', data: {namespace: testColl.getFullName(), numDocsToClone: 2}}),
              'numInitialSyncAttempts': 1
          }
        },
        true /* wait */);

    jsTestLog("Secondary was restarted");

    // Wait for failpoint to be reached so we know that collection cloning is paused.
    checkLog.contains(secondary, "initialSyncHangDuringCollectionClone fail point enabled");

    jsTestLog("Running operations while collection cloning is paused");

    // Run some operations on the sync source while collection cloning is paused so that we know
    // they must be applied during the oplog application stage of initial sync. This will also make
    // sure that the beginApplyingTimestamp and the stopTimestamp in initial sync are different. The
    // stopTimestamp is the timestamp of the oplog entry that was last applied on the sync source
    // when the oplog application phase of initial sync begins.
    const stopTimestamp =
        assert.commandWorked(testColl.runCommand("insert", {documents: [{_id: 4}]})).operationTime;

    jsTestLog("stopTimestamp: " + stopTimestamp);

    // Resume initial sync.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

    jsTestLog("Initial sync resumed");

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

    // TODO SERVER-36492: Step up the secondary, make sure that we get the prepare conflicts and
    // lock conflicts we expect, make sure we can commit the second transaction after initial sync
    // is done and that we can successfully run another transaction.

    jsTestLog("Aborting the second transaction");
    session2.abortTransaction_forTesting();

    replTest.stopSet();
})();

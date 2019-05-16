/**
 * Tests that initial sync properly fetches from the oldest active transaction timestamp, but that
 * it only applies from the beginApplyingTimestamp. The beginApplyingTimestamp is the timestamp of
 * the oplog entry that was last applied on the sync source before initial sync begins. It is also
 * the timestamp after which we will start applying operations during initial sync.
 *
 * To make sure that it is applying from the correct point, the test prepares a transaction before
 * the beginFetchingTimestamp and commits it before the beginApplyingTimestamp. Since the
 * transaction is not active by the time initial sync begins, its prepare oplog entry
 * won't be fetched during oplog application and trying to apply the commitTransaction oplog entry
 * will cause initial sync to fail.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";

    load("jstests/libs/check_log.js");
    load("jstests/core/txns/libs/prepare_helpers.js");

    const replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();

    const config = replTest.getReplSetConfig();
    // Increase the election timeout so that we do not accidentally trigger an election while the
    // secondary is restarting.
    config.settings = {"electionTimeoutMillis": 12 * 60 * 60 * 1000};
    replTest.initiate(config);

    const primary = replTest.getPrimary();
    let secondary = replTest.getSecondary();

    const dbName = "test";
    const collName = "initial_sync_fetch_from_oldest_active_transaction_timestamp";
    let testDB = primary.getDB(dbName);
    let testColl = testDB.getCollection(collName);

    assert.commandWorked(testColl.insert({_id: 1}));

    jsTestLog("Preparing a transaction that will later be committed");

    const session1 = primary.startSession();
    const sessionDB1 = session1.getDatabase(dbName);
    const sessionColl1 = sessionDB1.getCollection(collName);
    session1.startTransaction();
    assert.commandWorked(sessionColl1.insert({_id: 2}));
    const prepareTimestamp1 = PrepareHelpers.prepareTransaction(session1);

    jsTestLog("Preparing a transaction that will later be the oldest active transaction");

    // Prepare a transaction so that there is an active transaction with an oplog entry. The
    // timestamp of the first oplog entry of this transaction will become the beginFetchingTimestamp
    // during initial sync.
    let session2 = primary.startSession();
    let sessionDB2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDB2.getCollection(collName);
    session2.startTransaction();
    assert.commandWorked(sessionColl2.update({_id: 1}, {_id: 1, a: 1}));
    let prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2);

    const lsid2 = session2.getSessionId();
    const txnNumber2 = session2.getTxnNumber_forTesting();

    const oplog = primary.getDB("local").getCollection("oplog.rs");
    const txnNum = session2.getTxnNumber_forTesting();
    const op = oplog.findOne({"txnNumber": txnNum, "lsid.id": session2.getSessionId().id});
    assert.neq(op, null);
    const beginFetchingTs = op.ts;
    jsTestLog("Expected beginFetchingTimestamp: " + beginFetchingTs);

    // Commit the first transaction so that we have an operation that is fetched during initial sync
    // but should not be applied. If this is applied, initial sync will fail because while trying to
    // apply the commitTransaction oplog entry, it will fail to get the prepare oplog
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
    replTest.awaitReplication();

    jsTestLog("Initial sync completed");

    // Make sure the secondary fetched enough transaction oplog entries.
    secondary.setSlaveOk();
    const secondaryOplog = secondary.getDB("local").getCollection("oplog.rs");
    assert.eq(secondaryOplog.find({"ts": beginFetchingTs}).itcount(), 1);

    // Make sure the first transaction committed properly and is reflected after the initial sync.
    let res = secondary.getDB(dbName).getCollection(collName).findOne({_id: 2});
    assert.docEq(res, {_id: 2}, res);

    jsTestLog("Stepping up the secondary");

    // Step up the secondary after initial sync is done and make sure the transaction is properly
    // prepared.
    replTest.stepUp(secondary);
    replTest.waitForState(secondary, ReplSetTest.State.PRIMARY);
    const newPrimary = replTest.getPrimary();
    testDB = newPrimary.getDB(dbName);
    testColl = testDB.getCollection(collName);

    // Force the second session to use the same lsid and txnNumber as from before the restart. This
    // ensures that we're working with the same session and transaction.
    session2 = PrepareHelpers.createSessionWithGivenId(newPrimary, lsid2);
    session2.setTxnNumber_forTesting(txnNumber2);
    sessionDB2 = session2.getDatabase(dbName);

    jsTestLog("Checking that the second transaction is properly prepared");

    // Make sure that we can't read changes to the document from the second prepared transaction
    // after initial sync.
    assert.eq(testColl.find({_id: 1}).toArray(), [{_id: 1}]);

    // Make sure that another write on the same document from the second transaction causes a write
    // conflict.
    assert.commandFailedWithCode(
        testDB.runCommand(
            {update: collName, updates: [{q: {_id: 1}, u: {$set: {a: 2}}}], maxTimeMS: 5 * 1000}),
        ErrorCodes.MaxTimeMSExpired);

    // Make sure that we cannot add other operations to the second transaction since it is prepared.
    assert.commandFailedWithCode(sessionDB2.runCommand({
        insert: collName,
        documents: [{_id: 3}],
        txnNumber: NumberLong(txnNumber2),
        stmtId: NumberInt(2),
        autocommit: false
    }),
                                 ErrorCodes.PreparedTransactionInProgress);

    jsTestLog("Committing the second transaction");

    // Make sure we can successfully commit the second transaction after recovery.
    assert.commandWorked(sessionDB2.adminCommand({
        commitTransaction: 1,
        commitTimestamp: prepareTimestamp2,
        txnNumber: NumberLong(txnNumber2),
        autocommit: false
    }));
    assert.eq(testColl.find({_id: 1}).toArray(), [{_id: 1, a: 1}]);

    jsTestLog("Attempting to run another transaction");

    // Make sure that we can run another conflicting transaction without any problems.
    session2.startTransaction();
    assert.commandWorked(sessionDB2[collName].update({_id: 1}, {_id: 1, a: 2}));
    prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2);
    assert.commandWorked(PrepareHelpers.commitTransaction(session2, prepareTimestamp2));
    assert.eq(testColl.findOne({_id: 1}), {_id: 1, a: 2});

    replTest.stopSet();
})();

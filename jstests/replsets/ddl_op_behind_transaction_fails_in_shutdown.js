/**
 * Tests that a DDL op (drop collection) begun after a prepared transaction fails during shutdown.
 * Sets up a prepared transaction, starts drop collection in a parallel shell, attempts to wait for
 * the drop collection operation to enqueue a lock request waiting behind the transaction, then
 * shuts down the server. On restarting, we expect to find the collection was not dropped, that
 * there is no way for a DDL op to slip through and succeed after transaction resources (locks) are
 * cleared away during shutdown.
 *
 * This is an attempt to test that DDL ops cannot succeed AFTER transaction state (including locks)
 * is cleared during shutdown. However, a lot goes on during shutdown and the DDL op probably gets
 * interrupted well before the transaction state is cleared: either during shutdown's repl
 * stepdown attempt or as soon as operations get interrupted for shutdown (which occurs before
 * transaction state is cleared).
 *
 * @tags: [requires_persistence, uses_prepare_transaction, uses_transactions]
 */

(function() {
    "use strict";

    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/libs/check_log.js");
    load("jstests/libs/parallel_shell_helpers.js");
    load('jstests/libs/test_background_ops.js');

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const dbName = "test";
    const collName = "ddl_op_behind_prepared_transaction_fails_in_shutdown";
    let primary = rst.getPrimary();
    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);
    const txnDoc = {_id: 100};

    jsTest.log("Creating a collection '" + collName + "' with data in it...");
    assert.commandWorked(testDB.createCollection(collName));
    let bulk = testColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 2; ++i) {
        bulk.insert({_id: i});
    }
    assert.writeOK(bulk.execute());

    jsTest.log("Setting up a prepared transaction...");
    const session = primary.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(txnDoc));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    function runDropCollection(dbName, collName) {
        jsTest.log("Dropping collection in parallel shell...");
        // 'db' is defined in the parallel shell 'startParallelShell' will spin up.
        const res = db.getSiblingDB(dbName).runCommand({drop: collName});
        assert.commandFailedWithCode(
            res,
            [ErrorCodes.InterruptedAtShutdown, ErrorCodes.InterruptedDueToStepDown],
            "parallel shell drop cmd completed in an unexpected way: " + tojson(res));
        jsTest.log("Done dropping collection in parallel shell");
    };

    // Use a failpoint to wait for the drop operation to get as close as possible to a lock request
    // before we release it and wait 1 second more for it to hopefully have time to enqueue a lock
    // request. It takes a while for the parallel shell to start up, establish a connection with the
    // server for the drop operation, etc., and we do not want to interrupt it earlier than lock
    // acquisition with the shutdown signal.
    //
    // This is best-effort, not deterministic, since we cannot place a fail point directly in the
    // locking code as that would hang everything rather than just drop.
    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: 'hangDropCollectionBeforeLockAcquisition', mode: 'alwaysOn'}));
    let joinDropCollection;
    try {
        jsTest.log("Starting a parallel shell to concurrently run drop collection...");
        joinDropCollection =
            startParallelShell(funWithArgs(runDropCollection, dbName, collName), primary.port);

        jsTest.log("Waiting for drop collection to block behind the prepared transaction...");
        checkLog.contains(
            primary, "Hanging drop collection before lock acquisition while fail point is set");
    } finally {
        assert.commandWorked(primary.adminCommand(
            {configureFailPoint: 'hangDropCollectionBeforeLockAcquisition', mode: 'off'}));
    }
    sleep(1 * 1000);

    jsTest.log("Restarting the mongod...");
    // Skip validation because it requires a lock that the prepared transaction is blocking.
    rst.stop(primary, undefined, {skipValidation: true});
    rst.start(primary, {}, true /*restart*/);
    primary = rst.getPrimary();

    joinDropCollection();

    const numDocs = primary.getDB(dbName).getCollection(collName).find().length();
    // We expect two documents because the third is in an uncommitted transaction and not visible.
    assert.eq(
        2,
        numDocs,
        "Expected '" + collName + "' to find 2 documents, found " + numDocs +
            ". Drop collection may have succeeded during shutdown while a transaction was in the " +
            "prepared state.");

    // We will check that the prepared transaction is still active as expected, since we are here.
    assert.commandFailedWithCode(primary.getDB(dbName).runCommand({
        find: collName,
        filter: txnDoc,
        readConcern: {afterClusterTime: prepareTimestamp},
        maxTimeMS: 5000
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    // Skip validation because it requires a lock that the prepared transaction is blocking.
    rst.stopSet(true /*use default exit signal*/, false /*forRestart*/, {skipValidation: true});
})();

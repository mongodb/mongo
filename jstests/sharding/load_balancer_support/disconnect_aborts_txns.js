/**
 * @tags: [uses_transactions, uses_multi_shard_transaction,
 * requires_sharding]
 *
 * Tests that when a load-balanced client disconnects, its in-progress transactions are aborted
 */

(() => {
    "use strict";
    load("jstests/libs/parallelTester.js");

    function setupShardedCollection(st, dbName, collName) {
        const fullNss = dbName + "." + collName;
        const admin = st.s.getDB("admin");
        // Shard collection; ensure docs on each shard
        assert.commandWorked(admin.runCommand({enableSharding: dbName}));
        assert.commandWorked(admin.runCommand({movePrimary: dbName, to: st.shard0.shardName}));
        assert.commandWorked(admin.runCommand({shardCollection: fullNss, key: {_id: 1}}));
        assert.commandWorked(admin.runCommand({split: fullNss, middle: {_id: 0}}));
        assert.commandWorked(
            admin.runCommand({moveChunk: fullNss, find: {_id: 0}, to: st.shard1.shardName}));

        // Insert some docs on each shard
        let coll = admin.getSiblingDB(dbName).getCollection(collName);
        var bulk = coll.initializeUnorderedBulkOp();
        for (let i = -150; i < 150; i++) {
            bulk.insert({_id: i});
        }
        assert.commandWorked(bulk.execute());
    }

    function startTxn(host, dbName, collName, countdownLatch, appName) {
        jsTestLog("Starting transaction on alternate thread.");
        const newMongo = new Mongo(`mongodb://${host}/?appName=${appName}`);
        assert.commandWorked(newMongo.adminCommand(
            {configureFailPoint: "clientIsFromLoadBalancer", mode: "alwaysOn"}));
        // We manually generate a logical session and send it to the server explicitly, to prevent
        // the shell from making its own logical session object which will attempt to explicitly
        // abort the transaction on disconnection. In this way, we simulate a "hard partition"
        // between the shell and mongos with the open transaction, where the shell never sends
        // endSessions or abortTransaction commands on disconnect, to ensure the server cleans up
        // itself.
        const mySession = {id: UUID()};
        const myTxnNumber = NumberLong(0);
        const findInTxnCmd = {
            find: collName,
            batchSize: 1,
            lsid: mySession,
            txnNumber: myTxnNumber,
            startTransaction: true,
            autocommit: false
        };
        let cmdRes = newMongo.getDB(dbName).runCommand(findInTxnCmd);
        assert.commandWorked(cmdRes);
        countdownLatch.await();
        return [mySession, myTxnNumber];
    }

    let st = new ShardingTest({shards: 2, mongos: 1});
    const dbName = "foo";
    const collName = "bar";
    const admin = st.s.getDB("admin");

    setupShardedCollection(st, dbName, collName);
    let countdownLatch = new CountDownLatch(1);

    // capture txn statistics before opening and aborting the txn.
    const beforeTxnStats = admin.adminCommand({'serverStatus': 1}).transactions;

    const appName = "load_balanced_disconnect_aborts_txns";
    let txnStartingThread =
        new Thread(startTxn, st.s.host, dbName, collName, countdownLatch, appName);
    txnStartingThread.start();

    let idleSession = {};

    // Wait until we can see the txn opened by txnStartingThread, identified by the appName, as
    // idle.
    jsTestLog("Looking for txn opened by alternate thread");
    assert.soon(() => {
        const curopCursor = admin.aggregate([
            {$currentOp: {allUsers: true, idleCursors: true, localOps: true, idleSessions: true}},
            {$match: {type: "idleSession"}},
            {$match: {appName: appName}}
        ]);
        if (curopCursor.hasNext()) {
            idleSession = curopCursor.next();
            return true;
        }
        return false;
    }, "Couldn't find transaction opened by txnStartingThread.");
    jsTestLog("Found Txn opened by other thread!");

    // We've found the txn started by txnStartingThread.
    // We now join that thread, and therefore end its connection to the server.
    countdownLatch.countDown();
    txnStartingThread.join();

    const [sessionLsid, txnNumber] = txnStartingThread.returnData();

    jsTestLog("Verifying that the transaction we found was the one opened by the alternate thread");
    assert.eq(idleSession.lsid.id, sessionLsid.id);
    assert.eq(idleSession.transaction.parameters.txnNumber, txnNumber);

    jsTestLog("Ensure that the transaction has been killed.");
    // Make sure we can't find that txn anymore/it has been killed.
    const numPrevInterrupted = beforeTxnStats.abortCause.hasOwnProperty('Interrupted')
        ? beforeTxnStats.abortCause.Interrupted
        : 0;
    assert.soon(() => {
        const afterTxnStats = admin.adminCommand({'serverStatus': 1}).transactions;
        return (afterTxnStats.totalAborted == beforeTxnStats.totalAborted + 1) &&
            (afterTxnStats.abortCause.Interrupted, numPrevInterrupted + 1);
    });

    assert.commandWorked(
        admin.adminCommand({configureFailPoint: "clientIsFromLoadBalancer", mode: "off"}));

    st.stop();
})();

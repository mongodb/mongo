/**
 * Verifies that a node waits for the write done on stepup to become majority committed before
 * resuming coordinating transaction commits.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

// The UUID consistency check uses connections to shards cached on the ShardingTest object, but this
// test causes failovers on a shard, so the cached connection is not usable.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    'use strict';

    load('jstests/sharding/libs/sharded_transactions_helpers.js');  // for waitForFailpoint
    load('jstests/libs/write_concern_util.js');  // for stopping/restarting replication

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    let st = new ShardingTest({
        shards: 3,
        rs0: {nodes: 2},
        causallyConsistent: true,
        other: {
            mongosOptions: {verbose: 3},
        }
    });

    let coordinatorReplSetTest = st.rs0;
    let participant0 = st.shard0;
    let participant1 = st.shard1;
    let participant2 = st.shard2;

    let lsid = {id: UUID()};
    let txnNumber = 0;

    const runCommitThroughMongosInParallelShellExpectTimeOut = function() {
        const runCommitExpectTimeOutCode = "assert.commandFailedWithCode(db.adminCommand({" +
            "commitTransaction: 1, maxTimeMS: 1000 * 10, " + "lsid: " + tojson(lsid) + "," +
            "txnNumber: NumberLong(" + txnNumber + ")," + "stmtId: NumberInt(0)," +
            "autocommit: false," + "})," + "ErrorCodes.MaxTimeMSExpired);";
        return startParallelShell(runCommitExpectTimeOutCode, st.s.port);
    };

    const setUp = function() {
        // Create a sharded collection with a chunk on each shard:
        // shard0: [-inf, 0)
        // shard1: [0, 10)
        // shard2: [10, +inf)
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
        assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: participant0.shardName}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
        // TODO (SERVER-40594): Remove _waitForDelete once the range deleter doesn't block step down
        // if it enters a prepare conflict retry loop.
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: ns, find: {_id: 0}, to: participant1.shardName, _waitForDelete: true}));
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: ns, find: {_id: 10}, to: participant2.shardName, _waitForDelete: true}));

        // These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
        // from the shards starting, aborting, and restarting the transaction due to needing to
        // refresh after the transaction has started.
        assert.commandWorked(participant0.adminCommand({_flushRoutingTableCacheUpdates: ns}));
        assert.commandWorked(participant1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
        assert.commandWorked(participant2.adminCommand({_flushRoutingTableCacheUpdates: ns}));

        // Start a new transaction by inserting a document onto each shard.
        assert.commandWorked(st.s.getDB(dbName).runCommand({
            insert: collName,
            documents: [{_id: -5}, {_id: 5}, {_id: 15}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }));
    };
    setUp();

    let coordPrimary = coordinatorReplSetTest.getPrimary();
    let coordSecondary = coordinatorReplSetTest.getSecondary();

    // Make the commit coordination hang before writing the decision, and send commitTransaction.
    assert.commandWorked(coordPrimary.adminCommand({
        configureFailPoint: "hangBeforeWritingDecision",
        mode: "alwaysOn",
    }));
    let awaitResult = runCommitThroughMongosInParallelShellExpectTimeOut();
    waitForFailpoint("Hit hangBeforeWritingDecision failpoint", 1);

    // Stop replication on all nodes in the coordinator replica set so that the write done on stepup
    // cannot become majority committed, regardless of which node steps up.
    stopServerReplication([coordPrimary, coordSecondary]);

    // Induce the coordinator primary to step down.

    // The amount of time the node has to wait before becoming primary again.
    const stepDownSecs = 1;
    assert.commandWorked(coordPrimary.adminCommand({replSetStepDown: stepDownSecs, force: true}));

    assert.commandWorked(coordPrimary.adminCommand({
        configureFailPoint: "hangBeforeWritingDecision",
        mode: "off",
    }));

    // The router should retry commitTransaction against the new primary and time out waiting to
    // access the coordinator catalog.
    awaitResult();

    // Re-enable replication, so that the write done on stepup can become majority committed.
    restartReplSetReplication(coordinatorReplSetTest);

    // Now, commitTransaction should succeed.
    assert.commandWorked(st.s.adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false
    }));

    jsTest.log("Verify that the transaction was committed on all shards.");
    assert.eq(3, st.s.getDB(dbName).getCollection(collName).find().itcount());

    st.stop();
})();

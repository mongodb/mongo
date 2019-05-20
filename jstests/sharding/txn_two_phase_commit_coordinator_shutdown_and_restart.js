/**
 * Tests that if the coordinator goes down after it makes the decision to commit, that both the
 * stable timestamp is still able to advance on the participants and we are able to eventually
 * commit the transaction.
 *
 * This test restarts shard replica sets, so it requires a persistent storage engine.
 * @tags: [uses_transactions, uses_multi_shard_transaction, requires_persistence]
 */

// The UUID consistency check uses connections to shards cached on the ShardingTest object, but this
// test causes failovers on a shard, so the cached connection is not usable.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Since we expect prepared transactions in progress when shutting down a node, we will need to skip
// these validation checks.
TestData.skipCheckDBHashes = true;

(function() {
    'use strict';

    load('jstests/sharding/libs/sharded_transactions_helpers.js');
    load('jstests/libs/write_concern_util.js');

    const rs0_opts = {nodes: [{}, {}]};
    // Start the participant replSet with one node as a priority 0 node to avoid flip flopping.
    const rs1_opts = {nodes: [{}, {rsConfig: {priority: 0}}]};
    const st = new ShardingTest(
        {shards: {rs0: rs0_opts, rs1: rs1_opts}, mongos: 1, causallyConsistent: true});

    // Create a sharded collection:
    // shard0: [-inf, 0)
    // shard1: [0, inf)
    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard0.name);
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: 'test.user', middle: {x: 0}}));
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard1.name}));

    const testDB = st.s0.getDB('test');
    assert.commandWorked(testDB.runCommand({insert: 'user', documents: [{x: -10}, {x: 10}]}));

    const coordinatorReplSetTest = st.rs0;
    const participantReplSetTest = st.rs1;

    let coordinatorPrimaryConn = coordinatorReplSetTest.getPrimary();
    let participantPrimaryConn = participantReplSetTest.getPrimary();

    const lsid = {id: UUID()};
    let txnNumber = 0;
    const participantList = [{shardId: st.shard0.shardName}, {shardId: st.shard1.shardName}];

    // Build the following command as a string since we need to persist the lsid and the txnNumber
    // into the scope of the parallel shell.
    // assert.commandFailedWithCode(db.adminCommand({
    //     commitTransaction: 1,
    //     maxTimeMS: 2000 * 10,
    //     lsid: lsid,
    //     txnNumber: NumberLong(txnNumber),
    //     stmtId: NumberInt(0),
    //     autocommit: false,
    // }), ErrorCodes.MaxTimeMSExpired);
    const runCommitThroughMongosInParallelShellExpectTimeOut = function() {
        const runCommitExpectTimeOutCode = "assert.commandFailedWithCode(db.adminCommand({" +
            "commitTransaction: 1, maxTimeMS: 2000 * 10, " + "lsid: " + tojson(lsid) + "," +
            "txnNumber: NumberLong(" + txnNumber + ")," + "stmtId: NumberInt(0)," +
            "autocommit: false," + "})," + "ErrorCodes.MaxTimeMSExpired);";
        return startParallelShell(runCommitExpectTimeOutCode, st.s.port);
    };

    jsTest.log("Starting a cross-shard transaction");
    // Start a cross shard transaction through mongos.
    const updateDocumentOnShard0 = {q: {x: -1}, u: {"$set": {a: 1}}, upsert: true};

    const updateDocumentOnShard1 = {q: {x: 1}, u: {"$set": {a: 1}}, upsert: true};

    assert.commandWorked(testDB.runCommand({
        update: 'user',
        updates: [updateDocumentOnShard0, updateDocumentOnShard1],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        startTransaction: true
    }));

    jsTest.log("Turn on hangBeforeWritingDecision failpoint");
    // Make the commit coordination hang before writing the decision, and send commitTransaction.
    // The transaction on the participant will remain in prepare.
    assert.commandWorked(coordinatorPrimaryConn.adminCommand({
        configureFailPoint: "hangBeforeWritingDecision",
        mode: "alwaysOn",
    }));

    // Run commit through mongos in a parallel shell. This should timeout since we have set the
    // failpoint.
    runCommitThroughMongosInParallelShellExpectTimeOut();
    waitForFailpoint("Hit hangBeforeWritingDecision failpoint", 1 /* numTimes */);

    jsTest.log("Stopping coordinator shard");
    // Stop the mongods on the coordinator shard using the SIGTERM signal. We must skip validation
    // checks since we'll be shutting down a node with a prepared transaction.
    coordinatorReplSetTest.stopSet(15, true /* forRestart */, {skipValidation: true} /* opts */);

    // Once the coordinator has gone down, do a majority write on the participant while there is a
    // prepared transaction. This will ensure that the stable timestamp is able to advance since
    // this write must be in the committed snapshot.
    const session = participantPrimaryConn.startSession();
    const sessionDB = session.getDatabase("dummy");
    const sessionColl = sessionDB.getCollection("dummy");
    session.resetOperationTime_forTesting();
    assert.commandWorked(sessionColl.insert({dummy: 2}, {writeConcern: {w: "majority"}}));
    assert.neq(session.getOperationTime(), null);
    assert.neq(session.getClusterTime(), null);
    jsTest.log("Successfully completed majority write on participant");

    // Confirm that a majority read on the secondary includes the dummy write. This would mean that
    // the stable timestamp also advanced on the secondary.
    // In order to do this read with readConcern majority, we must use afterClusterTime with causal
    // consistency enabled.
    const participantSecondaryConn = participantReplSetTest.getSecondary();
    const secondaryDB = participantSecondaryConn.getDB("dummy");
    const res = secondaryDB.runCommand({
        find: "dummy",
        readConcern: {level: "majority", afterClusterTime: session.getOperationTime()},
    });
    assert.eq(res.cursor.firstBatch.length, 1);

    jsTest.log("Restarting coordinator");
    // Restarting the coordinator will reset the fail point.
    coordinatorReplSetTest.startSet({restart: true});
    coordinatorPrimaryConn = coordinatorReplSetTest.getPrimary();

    jsTest.log("Committing transaction");
    // Now, commitTransaction should succeed.
    assert.commandWorked(st.s.adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false
    }));

    st.stop();

})();

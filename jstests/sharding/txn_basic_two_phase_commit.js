/**
 * Exercises the coordinator commands logic by simulating a basic two phase commit and basic two
 * phase abort.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, uses_multi_shard_transaction]
 */
(function() {
    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    let st = new ShardingTest({shards: 3, causallyConsistent: true});

    let coordinator = st.shard0;
    let participant1 = st.shard1;
    let participant2 = st.shard2;

    // Create a sharded collection with a chunk on each shard:
    // shard0: [-inf, 0)
    // shard1: [0, 10)
    // shard2: [10, +inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: coordinator.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

    /**
     * Sends an insert on 'collName' to each shard with 'coordinator: true' on the first shard.
     *
     * lsid should be an object with format {id: <UUID>}
     * txnNumber should be an integer
     */
    let setUp = function(lsid, txnNumber) {
        // Simulate that the first statement in the transaction touched 'shard0', so mongos
        // forwarded the statement with 'coordinator: true' to shard0.
        assert.commandWorked(coordinator.getDB(dbName).runCommand({
            insert: collName,
            documents: [{_id: -5}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
            coordinator: true,
        }));

        // Simulate that some statement in the transaction touched shards 'shard1' and 'shard2'.
        assert.commandWorked(participant1.getDB(dbName).runCommand({
            insert: collName,
            documents: [{_id: 5}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }));
        assert.commandWorked(participant2.getDB(dbName).runCommand({
            insert: collName,
            documents: [{_id: 15}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }));
    };

    /**
     * Calls 'coordinateCommitTransaction' on the coordinator with a participant list
     * containing all the shards.
     *
     * lsid should be an object with format {id: <UUID>}
     * txnNumber should be an integer
     */
    let coordinateCommit = function(lsid, txnNumber) {
        assert.commandWorked(coordinator.adminCommand({
            coordinateCommitTransaction: 1,
            participants: [
                {shardId: coordinator.shardName},
                {shardId: participant1.shardName},
                {shardId: participant2.shardName}
            ],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false,
        }));
    };

    //
    // Test two-phase abort
    //

    let lsid = {id: UUID()};
    let txnNumber = 0;

    setUp(lsid, txnNumber);

    // Simulate that mongos sends 'prepare' with the coordinator's id to participant 1. This should
    // cause a voteCommit to be sent to the coordinator from participant 1, so that when the
    // coordinator receives voteAbort, it will also send abort to participant 1.
    assert.commandWorked(participant1.adminCommand({
        prepareTransaction: 1,
        coordinatorId: coordinator.shardName,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));

    // Simulate that participant 2 votes to abort.
    assert.commandWorked(coordinator.adminCommand({
        voteAbortTransaction: 1,
        shardId: participant2.shardName,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));

    // Manually abort the transaction on participant 2, since the coordinator will not send
    // abortTransaction to a participant that voted to abort.
    assert.commandWorked(participant2.adminCommand({
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));

    // Simulate that mongos sends the participant list to the coordinator, which should have already
    // aborted locally. The coordinator object will no longer exist and coordinateCommit will thus
    // return NoSuchTransaction.
    let error = assert.throws(function() {
        coordinateCommit(lsid, txnNumber);
    }, [], "Expected NoSuchTransaction error");

    assert.eq(error.code, 251 /*NoSuchTransaction*/, tojson(error));

    // Verify that the transaction was aborted on all shards.
    assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());

    //
    // Test two-phase commit
    //

    txnNumber++;

    setUp(lsid, txnNumber);

    // Simulate that mongos sends 'prepare' with the coordinator's id to the non-coordinator
    // participants.
    assert.commandWorked(participant1.adminCommand({
        prepareTransaction: 1,
        coordinatorId: coordinator.shardName,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));
    assert.commandWorked(participant2.adminCommand({
        prepareTransaction: 1,
        coordinatorId: coordinator.shardName,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));

    // Simulate that mongos sends the participant list to the coordinator.
    coordinateCommit(lsid, txnNumber);

    // Verify that the transaction was committed on all shards.
    // Use assert.soon(), because although coordinateCommitTransaction currently blocks until the
    // commit process is fully complete, it will eventually be changed to only block until the
    // decision is *written*, at which point the test can pass the operationTime returned by
    // coordinateCommitTransaction as 'afterClusterTime' in the read to ensure the read sees the
    // transaction's writes (TODO SERVER-37165).
    assert.soon(function() {
        return 3 === st.s.getDB(dbName).getCollection(collName).find().itcount();
    });

    st.stop();

})();

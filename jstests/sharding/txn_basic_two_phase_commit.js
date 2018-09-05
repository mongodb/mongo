/**
 * Exercises the coordinator commands logic by simulating a basic two phase commit and basic two
 * phase abort.
 */
(function() {
    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    let st = new ShardingTest({shards: 3});

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
     * Then calls 'coordinateCommitTransaction' on the coordinator with a participant list
     * containing all the shards.
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

        // Simulate that mongos sends the participant list to the coordinator.
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

    // Simulate that some participant votes to abort.
    assert.commandWorked(coordinator.adminCommand({
        voteAbortTransaction: 1,
        shardId: participant2.shardName,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));
    // Manually abort the transaction on this participant, since the coordinator will not send
    // abortTransaction to a participant that voted to abort.
    assert.commandWorked(participant2.adminCommand({
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));

    // Verify that the transaction was aborted on all shards.
    assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());

    //
    // Test two-phase commit
    //

    txnNumber++;

    setUp(lsid, txnNumber);

    // Simulate that all participants vote to commit.
    assert.commandWorked(coordinator.adminCommand({
        voteCommitTransaction: 1,
        shardId: participant1.shardName,
        prepareTimestamp: Timestamp(0, 0),
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));
    assert.commandWorked(coordinator.adminCommand({
        voteCommitTransaction: 1,
        shardId: participant2.shardName,
        prepareTimestamp: Timestamp(0, 0),
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));
    assert.commandWorked(coordinator.adminCommand({
        voteCommitTransaction: 1,
        shardId: coordinator.shardName,
        prepareTimestamp: Timestamp(0, 0),
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));

    // Verify that the transaction was committed on all shards.
    assert.eq(3, st.s.getDB(dbName).getCollection(collName).find().itcount());

    st.stop();

})();

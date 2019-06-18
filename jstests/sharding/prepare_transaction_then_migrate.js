/**
 * Test that a migration will:
 * 1. Ignore multi-statement transaction prepare conflicts in the clone phase, and
 * 2. Pick up the changes for prepared transactions in the transfer mods phase.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load('jstests/libs/chunk_manipulation_util.js');
    load('jstests/sharding/libs/sharded_transactions_helpers.js');

    const dbName = "test";
    const collName = "user";

    const staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    const st = new ShardingTest({shards: {rs0: {nodes: 1}, rs1: {nodes: 1}}});
    st.adminCommand({enableSharding: 'test'});
    st.ensurePrimaryShard('test', st.shard0.shardName);
    st.adminCommand({shardCollection: 'test.user', key: {_id: 1}});

    const session = st.s.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    assert.commandWorked(sessionColl.insert({_id: 1}));

    const lsid = {id: UUID()};
    const txnNumber = 0;
    const stmtId = 0;

    assert.commandWorked(st.s0.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: 2}, {_id: 5}, {_id: 15}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        startTransaction: true,
        autocommit: false,
    }));

    const res = assert.commandWorked(st.shard0.getDB(dbName).adminCommand({
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }));

    const joinMoveChunk = moveChunkParallel(
        staticMongod, st.s.host, {_id: 1}, null, 'test.user', st.shard1.shardName);

    // Wait for catchup to verify that the migration has exited the clone phase.
    waitForMigrateStep(st.shard1, migrateStepNames.catchup);

    assert.commandWorked(st.shard0.getDB(dbName).adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        commitTimestamp: res.prepareTimestamp,
    }));

    joinMoveChunk();

    assert.eq(sessionColl.find({_id: 2}).count(), 1);

    st.stop();
    MongoRunner.stopMongod(staticMongod);
})();

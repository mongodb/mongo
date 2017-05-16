// This test ensures that if one collection is its migration critical section, this won't stall
// operations for other sharded or unsharded collections

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    'use strict';

    var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    var st = new ShardingTest({mongos: 1, shards: 2});
    assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
    st.ensurePrimaryShard('TestDB', st.shard0.shardName);

    var testDB = st.s0.getDB('TestDB');

    assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.Coll0', key: {Key: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: 'TestDB.Coll0', middle: {Key: 0}}));

    var coll0 = testDB.Coll0;
    assert.writeOK(coll0.insert({Key: -1, Value: '-1'}));
    assert.writeOK(coll0.insert({Key: 1, Value: '1'}));

    assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.Coll1', key: {Key: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: 'TestDB.Coll1', middle: {Key: 0}}));

    var coll1 = testDB.Coll1;
    assert.writeOK(coll1.insert({Key: -1, Value: '-1'}));
    assert.writeOK(coll1.insert({Key: 1, Value: '1'}));

    // Ensure that coll0 has chunks on both shards so we can test queries against both donor and
    // recipient for Coll1's migration below
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: 'TestDB.Coll0', find: {Key: 1}, to: st.shard1.shardName}));

    // Pause the move chunk operation just before it leaves the critical section
    pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {Key: 1}, null, 'TestDB.Coll1', st.shard1.shardName);

    waitForMoveChunkStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

    // Ensure that operations for 'Coll0' are not stalled
    assert.eq(1, coll0.find({Key: {$lte: -1}}).itcount());
    assert.eq(1, coll0.find({Key: {$gte: 1}}).itcount());
    assert.writeOK(coll0.insert({Key: -2, Value: '-2'}));
    assert.writeOK(coll0.insert({Key: 2, Value: '2'}));
    assert.eq(2, coll0.find({Key: {$lte: -1}}).itcount());
    assert.eq(2, coll0.find({Key: {$gte: 1}}).itcount());

    // Ensure that operations for non-sharded collections are not stalled
    var collUnsharded = testDB.CollUnsharded;
    assert.eq(0, collUnsharded.find({}).itcount());
    assert.writeOK(collUnsharded.insert({TestKey: 0, Value: 'Zero'}));
    assert.eq(1, collUnsharded.find({}).itcount());

    unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

    joinMoveChunk();

    st.stop();
})();

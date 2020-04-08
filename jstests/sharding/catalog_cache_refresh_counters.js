/**
 * Test that operations that get blocked by catalog cache refreshes get logged to
 * shardingStatistics.
 */

(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');
load("jstests/sharding/libs/chunk_bounds_util.js");

let st = new ShardingTest({mongos: 2, shards: 2});
const configDB = st.s.getDB('config');
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const mongos0DB = st.s0.getDB(dbName);
const mongos1DB = st.s1.getDB(dbName);
const mongos0Coll = mongos0DB[collName];
const mongos1Coll = mongos1DB[collName];

let setUp = () => {
    /**
     * Sets up a test by moving chunks to such that one chunk is on each
     * shard, with the following distribution:
     *     shard0: [-inf, 0)
     *     shard1: [0, inf)
     */
    assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: ns, middle: {x: 0}}));

    flushRoutersAndRefreshShardMetadata(st, {ns});
};

let getOpsBlockedByRefresh = () => {
    return assert.commandWorked(st.s1.adminCommand({serverStatus: 1}))
        .shardingStatistics.catalogCache.operationsBlockedByRefresh;
};

let verifyBlockedOperationsChange = (oldOperationsCount, increasedOps) => {
    let newOperationsCount = getOpsBlockedByRefresh();
    increasedOps.forEach(op => {
        assert.eq(newOperationsCount[op.opType], oldOperationsCount[op.opType] + op.increase);
    });
};

let getShardToTargetForMoveChunk = () => {
    const chunkDocs = configDB.chunks.find({ns: ns}).toArray();
    const shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunkDocs);
    const shardThatOwnsChunk = chunkBoundsUtil.findShardForShardKey(st, shardChunkBounds, {x: 100});
    return st.getOther(shardThatOwnsChunk).shardName;
};

let runTest = (operationToRunFn, expectedOpIncreases) => {
    let opsBlockedByRefresh = getOpsBlockedByRefresh();

    // Move chunk to ensure stale shard version for the next operation.
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: ns, find: {x: 100}, to: getShardToTargetForMoveChunk()}));

    operationToRunFn();
    verifyBlockedOperationsChange(opsBlockedByRefresh, expectedOpIncreases);
};

setUp();

/**
 * Verify that insert operations get logged when blocked by a refresh.
 */
runTest(() => assert.commandWorked(mongos1Coll.insert({x: 250})), [
    {opType: 'countAllOperations', increase: 2},
    {opType: 'countInserts', increase: 1},
    {opType: 'countCommands', increase: 1}
]);

/**
 * Verify that queries get logged when blocked by a refresh.
 */
runTest(() => mongos1Coll.findOne({x: 250}),
        [{opType: 'countAllOperations', increase: 1}, {opType: 'countQueries', increase: 1}]);

/**
 * Verify that updates get logged when blocked by a refresh.
 */
runTest(() => assert.commandWorked(mongos1Coll.update({x: 250}, {$set: {a: 1}})), [
    {opType: 'countAllOperations', increase: 2},
    {opType: 'countUpdates', increase: 1},
    {opType: 'countCommands', increase: 1}
]);

/**
 * Verify that deletes get logged when blocked by a refresh.
 */
runTest(() => assert.commandWorked(mongos1Coll.remove({x: 250})), [
    {opType: 'countAllOperations', increase: 2},
    {opType: 'countDeletes', increase: 1},
    {opType: 'countCommands', increase: 1}
]);

/**
 * Verify that non-CRUD commands get logged when blocked by a refresh.
 */
runTest(() => assert.commandWorked(mongos1DB.runCommand({collMod: collName})), [
    {opType: 'countAllOperations', increase: 1},
    {opType: 'countCommands', increase: 1},
]);

st.stop();
})();

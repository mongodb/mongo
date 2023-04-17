/**
 * Tests aggregate $out, $merge, $lookup and $unionWith under readConcern level snapshot outside of
 * transactions.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/global_snapshot_reads_util.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");
load("jstests/sharding/libs/find_chunks_util.js");

const nodeOptions = {
    // Set a large snapshot window of 10 minutes for the test.
    setParameter: {minSnapshotHistoryWindowInSeconds: 600}
};

const dbName = "test";
// The collection is not sharded.
const unshardedColl1 = "unshardedColl1";
const unshardedColl2 = "unshardedColl2";
// The sharded collection lives in a single shard.
const singleShardedColl1 = "singleShardedColl1";
const singleShardedColl2 = "singleShardedColl2";
// The sharded collection lives in 2 out of the 3 shards.
const someShardedColl1 = "someShardedColl1";
const someShardedColl2 = "someShardedColl2";
// The sharded collection lives in all 3 shards.
const allShardedColl1 = "allShardedColl1";
const allShardedColl2 = "allShardedColl2";

const st = new ShardingTest({
    shards: {
        rs0: {nodes: 2},
        rs1: {nodes: 2},
        rs2: {nodes: 2},
    },
    mongos: 1,
    config: TestData.configShard ? undefined : 1,
    other: {configOptions: nodeOptions, rsOptions: nodeOptions}
});
// Config sharded collections.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand(
    {shardCollection: st.s.getDB(dbName)[singleShardedColl1] + "", key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand(
    {shardCollection: st.s.getDB(dbName)[singleShardedColl2] + "", key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({shardCollection: st.s.getDB(dbName)[someShardedColl1] + "", key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({shardCollection: st.s.getDB(dbName)[someShardedColl2] + "", key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({shardCollection: st.s.getDB(dbName)[allShardedColl1] + "", key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({shardCollection: st.s.getDB(dbName)[allShardedColl2] + "", key: {_id: 1}}));

const mongos = st.s0;

// Set up someShardedColl.
const setupSomeShardedColl = (collName) => {
    let ns = dbName + '.' + collName;
    // snapshotReadsTest() inserts ids 0-9 and tries snapshot reads on the collection.
    assert.commandWorked(st.splitAt(ns, {_id: 5}));
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {_id: 7}, to: st.shard2.shardName}));

    assert.eq(
        0,
        findChunksUtil.countChunksForNs(mongos.getDB('config'), ns, {shard: st.shard0.shardName}));
    assert.eq(
        1,
        findChunksUtil.countChunksForNs(mongos.getDB('config'), ns, {shard: st.shard1.shardName}));
    assert.eq(
        1,
        findChunksUtil.countChunksForNs(mongos.getDB('config'), ns, {shard: st.shard2.shardName}));
    flushRoutersAndRefreshShardMetadata(st, {ns});
};
setupSomeShardedColl(someShardedColl1);
setupSomeShardedColl(someShardedColl2);

// Set up allShardedColl.
const setupAllShardedColl = (collName) => {
    let ns = dbName + '.' + collName;
    // snapshotReadsTest() inserts ids 0-9 and tries snapshot reads on the collection.
    assert.commandWorked(st.splitAt(ns, {_id: 4}));
    assert.commandWorked(st.splitAt(ns, {_id: 7}));

    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {_id: 4}, to: st.shard1.shardName}));
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {_id: 7}, to: st.shard2.shardName}));

    assert.eq(
        1,
        findChunksUtil.countChunksForNs(mongos.getDB('config'), ns, {shard: st.shard0.shardName}));
    assert.eq(
        1,
        findChunksUtil.countChunksForNs(mongos.getDB('config'), ns, {shard: st.shard1.shardName}));
    assert.eq(
        1,
        findChunksUtil.countChunksForNs(mongos.getDB('config'), ns, {shard: st.shard2.shardName}));
    flushRoutersAndRefreshShardMetadata(st, {ns});
};
setupAllShardedColl(allShardedColl1);
setupAllShardedColl(allShardedColl2);

function awaitCommittedFn() {
    for (let i = 0; st['rs' + i] !== undefined; i++) {
        st['rs' + i].awaitLastOpCommitted();
    }
}

// Pass the same DB handle as "primaryDB" and "secondaryDB" params; the test functions will
// send readPreference to mongos to target primary/secondary shard servers.
let db = st.s.getDB(dbName);
let snapshotReadsTest =
    new SnapshotReadsTest({primaryDB: db, secondaryDB: db, awaitCommittedFn: awaitCommittedFn});

for (let coll1 of [unshardedColl1, singleShardedColl1, someShardedColl1, allShardedColl1]) {
    for (let coll2 of [unshardedColl2, singleShardedColl2, someShardedColl2, allShardedColl2]) {
        const scenarioName = `${coll1}+${coll2}`;
        snapshotReadsTest.outAndMergeTest({
            testScenarioName: scenarioName,
            coll: coll1,
            outColl: coll2,
            isOutCollSharded: (coll2 !== unshardedColl2)
        });
        snapshotReadsTest.lookupAndUnionWithTest({
            testScenarioName: scenarioName,
            coll1: coll1,
            coll2: coll2,
            isColl2Sharded: (coll2 !== unshardedColl2)
        });
    }
}

st.stop();
})();

/**
 * Test that chunk operations cause the mongos to refresh if the finer grained catalog cache
 * refresh flag is set to false.
 */

(function() {
'use strict';

load("jstests/sharding/libs/shard_versioning_util.js");
load('jstests/sharding/libs/sharded_transactions_helpers.js');

let st = new ShardingTest({
    mongos: 1,
    shards: 3,
    mongosOptions: {setParameter: {enableFinerGrainedCatalogCacheRefresh: false}}
});
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let getMongosCollVersion = (ns) => {
    return st.s.adminCommand({getShardVersion: ns}).version;
};

let setUp = () => {
    /**
     * Sets up a test by moving chunks to such that one chunk is on each
     * shard, with the following distribution:
     *     shard0: [-inf, -10)
     *     shard1: [-10, 10)
     *     shard2: [10, inf)
     */
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -10}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 10}}));

    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 1000}, to: st.shard2.shardName}));
    flushRoutersAndRefreshShardMetadata(st, {ns});
};

// Verify that a split updates the mongos' catalog cache.
let testSplit = () => {
    const mongosCollectionVersion = getMongosCollVersion(ns);
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -500}}));
    assert.lt(mongosCollectionVersion, getMongosCollVersion(ns));
};

// Verify that a merge updates the mongos' catalog cache.
let testMerge = () => {
    const mongosCollectionVersion = getMongosCollVersion(ns);
    assert.commandWorked(st.s.adminCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: -10}]}));
    assert.lt(mongosCollectionVersion, getMongosCollVersion(ns));
};

// Verify that a chunk move updates the mongos' catalog cache.
let testMoveChunk = () => {
    let mongosCollectionVersion = getMongosCollVersion(ns);

    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard1.shardName}));
    assert.lt(mongosCollectionVersion, getMongosCollVersion(ns));

    mongosCollectionVersion = getMongosCollVersion(ns);

    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard0.shardName}));
    assert.lt(mongosCollectionVersion, getMongosCollVersion(ns));
};

setUp();
testSplit();
testMerge();
testMoveChunk();

st.stop();
})();

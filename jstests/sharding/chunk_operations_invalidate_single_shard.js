/**
 * Test that chunk operations don't cause the mongos to refresh unless an affected chunk is
 * targeted.
 * @tags: [requires_fcv_44]
 */

(function() {
'use strict';

load("jstests/sharding/libs/shard_versioning_util.js");
load('jstests/sharding/libs/sharded_transactions_helpers.js');

let st = new ShardingTest({
    mongos: 1,
    shards: 3,
    other: {mongosOptions: {setParameter: {enableFinerGrainedCatalogCacheRefresh: true}}}
});
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
let testDB = st.s.getDB(dbName);
let testColl = testDB.foo;

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

// Verify that a split doesn't update the mongos' catalog cache unless an affected chunk is
// targeted.
let testSplit = () => {
    const mongosCollectionVersion = getMongosCollVersion(ns);

    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -500}}));

    testColl.findOne({x: 0});
    testColl.findOne({x: 1000});
    assert.eq(mongosCollectionVersion, getMongosCollVersion(ns));

    testColl.findOne({x: -1000});
    assert.lt(mongosCollectionVersion, getMongosCollVersion(ns));
};

// Verify that a merge doesn't update the mongos' catalog cache unless an affected chunk is
// targeted.
let testMerge = () => {
    const mongosCollectionVersion = getMongosCollVersion(ns);

    assert.commandWorked(st.s.adminCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: -10}]}));
    testColl.findOne({x: 0});
    testColl.findOne({x: 1000});
    assert.eq(mongosCollectionVersion, getMongosCollVersion(ns));

    testColl.findOne({x: -1000});
    assert.lt(mongosCollectionVersion, getMongosCollVersion(ns));
};

// Verify that a chunk move doesn't update the mongos' catalog cache unless an affected chunk is
// targeted.
let testMoveChunk = () => {
    // Contact the donor shard to trigger update.
    let mongosCollectionVersion = getMongosCollVersion(ns);

    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard1.shardName}));
    testColl.findOne({x: 1000});
    assert.eq(mongosCollectionVersion, getMongosCollVersion(ns));

    testColl.findOne({x: -1000});
    assert.lt(mongosCollectionVersion, getMongosCollVersion(ns));

    // Contact the recipient shard to trigger update.
    mongosCollectionVersion = getMongosCollVersion(ns);

    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard0.shardName}));
    testColl.findOne({x: 1000});
    assert.eq(mongosCollectionVersion, getMongosCollVersion(ns));

    testColl.findOne({x: -1000});
    assert.lt(mongosCollectionVersion, getMongosCollVersion(ns));
};

setUp();
testSplit();
testMerge();
testMoveChunk();

st.stop();
})();

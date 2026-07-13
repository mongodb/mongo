/**
 * Test that for an unsharded collection the listIndexes command targets the database's primary
 * shard, and for a sharded collection the command sends and checks shard versions and only
 * targets the shard that owns the MinKey chunk (refreshing and retargeting on a stale routing
 * table).
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

// This test makes shards have inconsistent indexes.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on shards and cause them to refresh their sharding metadata.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false},
};

const st = new ShardingTest({mongos: 2, shards: 2, other: {configOptions: nodeOptions}});

const dbName = "test";
const collName = "user";
const ns = dbName + "." + collName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

st.shard0.getCollection(ns).createIndexes([{a: 1}]);

// Assert that listIndexes targets the primary shard for an unsharded collection.
let indexes = st.s.getCollection(ns).getIndexes();
indexes.sort(bsonWoCompare);
assert.eq(2, indexes.length);
assert.eq(
    0,
    bsonWoCompare({_id: 1}, indexes[0].key),
    `expected listIndexes to return index {_id: 1} but found: ${tojson(indexes)}`,
);
assert.eq(
    0,
    bsonWoCompare({a: 1}, indexes[1].key),
    `expected listIndexes to return index {a: 1} but found: ${tojson(indexes)}`,
);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// The collection has a single (MinKey) chunk, which starts on shard0. Set up a scenario where the
// router that issues listIndexes (st.s) believes the chunk is still on shard0, while it has actually
// been moved to shard1 through a different router (st.s1). A correct listIndexes must send a shard
// version to shard0, receive a StaleConfig, refresh, and retarget shard1.
//
// shard0 (the stale target) keeps its 2 indexes ({_id: 1}, {a: 1}); shard1 (the true owner) is
// given a third index ({c: 1}) after receiving the chunk. Reporting shard1's 3 indexes proves the
// command refreshed and retargeted rather than reading from the stale shard0. A shard can never be
// ignorant of its own metadata under authoritative shards, so the staleness must live on the router.
indexes = ShardVersioningUtil.runOperationOnStaleRouterAfterMoveChunk({
    migrateRouter: st.s1,
    staleRouter: st.s,
    ns,
    toShard: st.shard1,
    bounds: [{_id: MinKey}, {_id: MaxKey}],
    staleOperation: (router) => {
        // shard1 received {_id: 1} and {a: 1} with the chunk; give it a distinguishing third index.
        assert.commandWorked(st.shard1.getCollection(ns).createIndexes([{c: 1}]));
        return router.getCollection(ns).getIndexes();
    },
});

// Assert that listIndexes refreshed and targeted the shard that owns the MinKey chunk (shard1).
indexes.sort(bsonWoCompare);
assert.eq(3, indexes.length, `expected 3 indexes but found: ${tojson(indexes)}`);
assert.eq(
    0,
    bsonWoCompare({_id: 1}, indexes[0].key),
    `expected listIndexes to return index {_id: 1} but found: ${tojson(indexes)}`,
);
assert.eq(
    0,
    bsonWoCompare({a: 1}, indexes[1].key),
    `expected listIndexes to return index {a: 1} but found: ${tojson(indexes)}`,
);
assert.eq(
    0,
    bsonWoCompare({c: 1}, indexes[2].key),
    `expected listIndexes to return index {c: 1} but found: ${tojson(indexes)}`,
);

st.stop();

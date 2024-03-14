/**
 * Test that for an unsharded collection the listIndexes command targets the database's primary
 * shard, and for a sharded collection the command sends and checks shard versions and only
 * targets the shard that owns the MinKey chunk.
 */
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

// This test makes shards have inconsistent indexes.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on shards and cause them to refresh their sharding metadata.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false}
};

const st = new ShardingTest({shards: 3, other: {configOptions: nodeOptions}});
const dbName = "test";
const collName = "user";
const ns = dbName + "." + collName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

st.shard0.getCollection(ns).createIndexes([{a: 1}]);

// Assert that listIndexes targets the primary shard for an unsharded collection.
let indexes = st.s.getCollection(ns).getIndexes();
indexes.sort(bsonWoCompare);
assert.eq(2, indexes.length);
assert.eq(0,
          bsonWoCompare({_id: 1}, indexes[0].key),
          `expected listIndexes to return index {_id: 1} but found: ${tojson(indexes)}`);
assert.eq(0,
          bsonWoCompare({a: 1}, indexes[1].key),
          `expected listIndexes to return index {a: 1} but found: ${tojson(indexes)}`);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Perform a series of chunk operations to make the shards have the following chunks:
// shard0: [0, MaxKey)
// shard1: [null, 0)
// shard2: [MinKey, null)
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
ShardVersioningUtil.moveChunkNotRefreshRecipient(st.s, ns, st.shard0, st.shard1, {_id: MinKey});

assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: null}}));
ShardVersioningUtil.moveChunkNotRefreshRecipient(st.s, ns, st.shard1, st.shard2, {_id: MinKey});

const latestCollectionVersion = ShardVersioningUtil.getMetadataOnShard(st.shard1, ns).collVersion;

// Assert that all non-donor shards have a stale collection version.
ShardVersioningUtil.assertCollectionVersionOlderThan(st.shard0, ns, latestCollectionVersion);
ShardVersioningUtil.assertCollectionVersionEquals(st.shard1, ns, latestCollectionVersion);
ShardVersioningUtil.assertCollectionVersionOlderThan(st.shard2, ns, latestCollectionVersion);

// Create indexes directly on the other shards.
st.shard1.getCollection(ns).createIndexes([{b: 1}]);
st.shard2.getCollection(ns).createIndexes([{c: 1}]);

indexes = st.s.getCollection(ns).getIndexes();

// Assert that listIndexes only targeted the shard with the MinKey chunk (shard2).
indexes.sort(bsonWoCompare);
assert.eq(3, indexes.length);
assert.eq(0,
          bsonWoCompare({_id: 1}, indexes[0].key),
          `expected listIndexes to return index {_id: 1} but found: ${tojson(indexes)}`);
assert.eq(0,
          bsonWoCompare({a: 1}, indexes[1].key),
          `expected listIndexes to return index {a: 1} but found: ${tojson(indexes)}`);
assert.eq(0,
          bsonWoCompare({c: 1}, indexes[2].key),
          `expected listIndexes to return index {c: 1} but found: ${tojson(indexes)}`);

st.stop();
/**
 * Tests for basic functionality of the move collection feature.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection,
 *  # TODO (SERVER-87812) Remove multiversion_incompatible tag
 *  multiversion_incompatible,
 *  assumes_balancer_off,
 * ]
 */

(function() {
'use strict';

var st = new ShardingTest({mongos: 1, shards: 2});

const dbName = 'db';
const collName = 'foo';
const ns = dbName + '.' + collName;
let mongos = st.s0;
let shard0 = st.shard0.shardName;
let shard1 = st.shard1.shardName;

let cmdObj = {moveCollection: ns, toShard: shard0};

// Fail if collection is not tracked.
assert.commandFailedWithCode(mongos.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

// Fail if collection is sharded.
assert.commandFailedWithCode(mongos.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

// TODO (SERVER-86295) Replace createUnsplittableCollection with create once moveCollection
// registers the collection on the sharding catalog
const unsplittableCollName = "foo_unsplittable"
const unsplittableCollNs = dbName + '.' + unsplittableCollName;
assert.commandWorked(
    st.s.getDB(dbName).runCommand({createUnsplittableCollection: unsplittableCollName}));

// Fail if missing required field toShard.
assert.commandFailedWithCode(mongos.adminCommand({moveCollection: unsplittableCollNs}),
                             ErrorCodes.IDLFailedToParse);

// Fail if command called on shard.
assert.commandFailedWithCode(
    st.shard0.adminCommand({moveCollection: unsplittableCollNs, toShard: shard1}),
    ErrorCodes.CommandNotFound);

const coll = mongos.getDB(dbName)[unsplittableCollName];
for (let i = -25; i < 25; ++i) {
    assert.commandWorked(coll.insert({oldKey: i}));
}
assert.eq(50, st.rs0.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));

// move to non-primary shard.
assert.commandWorked(mongos.adminCommand({moveCollection: unsplittableCollNs, toShard: shard1}));

// Should have unsplittable set to true
let configDb = mongos.getDB('config');
let unshardedColl = configDb.collections.findOne({_id: unsplittableCollNs});
assert.eq(unshardedColl.unsplittable, true);
let unshardedChunk = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
assert.eq(1, unshardedChunk.length);

assert.eq(50, st.rs1.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));
assert.eq(0, st.rs0.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));

const metrics = st.config0.getDB('admin').serverStatus({}).shardingStatistics.moveCollection;

assert.eq(metrics.countStarted, 1);
assert.eq(metrics.countSameKeyStarted, undefined);
assert.eq(metrics.countSucceeded, 1);
assert.eq(metrics.countFailed, 0);
assert.eq(metrics.countCanceled, 0);

// move to primary shard.
assert.commandWorked(mongos.adminCommand({moveCollection: unsplittableCollNs, toShard: shard0}));
unshardedColl = configDb.collections.findOne({_id: unsplittableCollNs});
assert.eq(unshardedColl.unsplittable, true);
unshardedChunk = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
assert.eq(1, unshardedChunk.length);

assert.eq(0, st.rs1.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));
assert.eq(50, st.rs0.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));

st.stop();
})();

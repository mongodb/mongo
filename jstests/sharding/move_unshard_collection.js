/**
 * Tests unshard and move collection behaviour.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection,
 *  featureFlagUnshardCollection,
 *  featureFlagTrackUnshardedCollectionsUponCreation,
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
const mongos = st.s;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

assert.commandWorked(mongos.adminCommand({split: ns, middle: {oldKey: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: -1}, to: shard0}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: 10}, to: shard1}));

const coll = mongos.getDB(dbName)[collName];
for (let i = -25; i < 25; ++i) {
    assert.commandWorked(coll.insert({oldKey: i}));
}

assert.commandFailedWithCode(mongos.adminCommand({moveCollection: ns, toShard: shard1}),
                             ErrorCodes.NamespaceNotFound);

jsTest.log('Unshard the sharded collection to a non-primary shard.');
assert.commandWorked(mongos.adminCommand({unshardCollection: ns, toShard: shard1}));

assert.eq(50, st.rs1.getPrimary().getCollection(ns).countDocuments({}));
assert.eq(0, st.rs0.getPrimary().getCollection(ns).countDocuments({}));

function checkIsUnsplittableSet() {
    let configDb = mongos.getDB('config');
    let unshardedColl = configDb.collections.findOne({_id: ns});
    assert.eq(unshardedColl.unsplittable, true);
}

let testKeyVal = 1000;

function testReadWriteSucceeds() {
    let result = coll.findOne({oldKey: 1});

    assert.commandWorked(coll.insert({testKey: testKeyVal}));
    result = coll.findOne({testKey: testKeyVal});
    ++testKeyVal;
}

checkIsUnsplittableSet();
testReadWriteSucceeds();

assert.commandFailedWithCode(mongos.adminCommand({unshardCollection: ns, toShard: shard0}),
                             ErrorCodes.NamespaceNotSharded);

jsTest.log('Move the unsharded collection to the primary shard.');
assert.commandWorked(mongos.adminCommand({moveCollection: ns, toShard: shard0}));

assert.eq(0, st.rs1.getPrimary().getCollection(ns).countDocuments({}));
assert.eq(51, st.rs0.getPrimary().getCollection(ns).countDocuments({}));

checkIsUnsplittableSet();
testReadWriteSucceeds();

st.stop();
})();

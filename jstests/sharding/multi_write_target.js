//
// Tests that multi-writes (update/delete) target *all* shards and not just shards in the collection
//

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    WriteWithoutShardKeyTestUtil
} from "jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js";

var st = new ShardingTest({shards: 3, mongos: 2});

var admin = st.s0.getDB("admin");
var coll = st.s0.getCollection("foo.bar");

assert.commandWorked(
    admin.runCommand({enableSharding: coll.getDB() + "", primaryShard: st.shard0.shardName}));
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {skey: 1}}));

assert.commandWorked(admin.runCommand({split: coll + "", middle: {skey: 0}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {skey: 100}}));
assert.commandWorked(
    admin.runCommand({moveChunk: coll + "", find: {skey: 0}, to: st.shard1.shardName}));
assert.commandWorked(
    admin.runCommand({moveChunk: coll + "", find: {skey: 100}, to: st.shard2.shardName}));

jsTest.log("Testing multi-update...");

// Put data on all shards
assert.commandWorked(st.s0.getCollection(coll.toString()).insert({_id: 0, skey: -1, x: 1}));
assert.commandWorked(st.s0.getCollection(coll.toString()).insert({_id: 1, skey: 1, x: 1}));
assert.commandWorked(st.s0.getCollection(coll.toString()).insert({_id: 0, skey: 100, x: 1}));

// Sharded updateOnes that do not directly target a shard can now use the two phase write
// protocol to execute.
if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    assert.commandWorked(coll.update({x: 1}, {$set: {updated: true}}, {multi: false}));
} else {
    // Non-multi-update doesn't work without shard key
    assert.commandFailedWithCode(coll.update({x: 1}, {$set: {updated: true}}, {multi: false}),
                                 ErrorCodes.InvalidOptions);
}

assert.commandWorked(coll.update({x: 1}, {$set: {updated: true}}, {multi: true}));

// Ensure update goes to *all* shards
assert.neq(null, st.shard0.getCollection(coll.toString()).findOne({updated: true}));
assert.neq(null, st.shard1.getCollection(coll.toString()).findOne({updated: true}));
assert.neq(null, st.shard2.getCollection(coll.toString()).findOne({updated: true}));

// _id update works, and goes to all shards even on the stale mongos
var staleColl = st.s1.getCollection('foo.bar');
assert.commandWorked(staleColl.update({_id: 0}, {$set: {updatedById: true}}, {multi: false}));

if (FeatureFlagUtil.isPresentAndEnabled(st.s, "UpdateOneWithIdWithoutShardKey")) {
    // Ensure _id update goes to at least one shard
    assert(st.shard0.getCollection(coll.toString()).findOne({updatedById: true}) != null ||
           st.shard2.getCollection(coll.toString()).findOne({updatedById: true}) != null)
} else {
    // Ensure _id update goes to all shards
    assert.neq(null, st.shard0.getCollection(coll.toString()).findOne({updatedById: true}));
    assert.neq(null, st.shard2.getCollection(coll.toString()).findOne({updatedById: true}));
}
jsTest.log("Testing multi-delete...");

// Sharded deleteOnes that do not directly target a shard can now use the two phase write
// protocol to execute.
if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    assert.commandWorked(coll.remove({x: 1}, {justOne: true}));
} else {
    // non-multi-delete doesn't work without shard key
    assert.commandFailedWithCode(coll.remove({x: 1}, {justOne: true}), ErrorCodes.ShardKeyNotFound);
}

assert.commandWorked(coll.remove({x: 1}, {justOne: false}));

// Ensure delete goes to *all* shards
assert.eq(null, st.shard0.getCollection(coll.toString()).findOne({x: 1}));
assert.eq(null, st.shard1.getCollection(coll.toString()).findOne({x: 1}));
assert.eq(null, st.shard2.getCollection(coll.toString()).findOne({x: 1}));

// Put more on all shards
assert.commandWorked(st.shard0.getCollection(coll.toString()).insert({_id: 0, skey: -1, x: 1}));
assert.commandWorked(st.shard1.getCollection(coll.toString()).insert({_id: 1, skey: 1, x: 1}));
assert.commandWorked(st.shard2.getCollection(coll.toString()).insert({_id: 0, skey: 100, x: 1}));

assert.commandWorked(coll.remove({_id: 0}, {justOne: true}));

if (FeatureFlagUtil.isPresentAndEnabled(st.s, "UpdateOneWithIdWithoutShardKey")) {
    // Ensure _id deleteOne goes to at least one shard
    assert(st.shard0.getCollection(coll.toString()).findOne({x: 1}) == null ||
           st.shard2.getCollection(coll.toString()).findOne({x: 1}) == null)
} else {
    // Ensure _id update goes to all shards
    assert.eq(null, st.shard0.getCollection(coll.toString()).findOne({x: 1}));
    assert.eq(null, st.shard2.getCollection(coll.toString()).findOne({x: 1}));
}

st.stop();

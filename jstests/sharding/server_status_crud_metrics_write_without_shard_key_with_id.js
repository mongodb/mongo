/**
 * Tests for the 'metrics.query' section of the mongoS serverStatus response for writes with _id
 * specified but not the shard key.
 *
 * @tags: [featureFlagUpdateOneWithIdWithoutShardKey, requires_fcv_80]
 */

import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({shards: 2});
const testDB = st.s.getDB("test");
const testColl = testDB.coll;

Random.setRandomSeed();

assert.commandWorked(
    testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.rs0.getURL()}));

CreateShardedCollectionUtil.shardCollectionWithChunks(testColl, {x: 1}, [
    {min: {x: MinKey}, max: {x: -100}, shard: st.shard0.shardName},
    {min: {x: -100}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

// Move chunk from shard0 to shard1.
assert.commandWorked(testDB.adminCommand(
    {moveChunk: testColl.getFullName(), find: {x: 1}, to: st.shard1.shardName}));

const session = st.s.startSession({retryWrites: true});
const sessionColl = session.getDatabase(testDB.getName()).getCollection(testDB.coll.getName());

// Insert five documents.
for (var i = 0; i < 5; i++) {
    assert.commandWorked(sessionColl.insert({x: -100 + Random.randInt(100), _id: i}));
}

assert.commandWorked(sessionColl.updateOne({_id: 0}, {$inc: {a: 1}}));
assert.commandWorked(sessionColl.deleteOne({_id: 1}));

session.endSession();

testColl.updateOne({_id: 2}, {$inc: {a: 1}});
testColl.deleteOne({_id: 3});

let mongosServerStatus = testDB.adminCommand({serverStatus: 1});

// Verify that metrics are incremented correctly for retryable writes without shard key with _id.
assert.eq(1, mongosServerStatus.metrics.query.updateOneWithoutShardKeyWithIdCount);
assert.eq(1, mongosServerStatus.metrics.query.deleteOneWithoutShardKeyWithIdCount);

// Verify that metrics are incremented correctly for non-retryable writes without shard key with
// _id.
assert.eq(1, mongosServerStatus.metrics.query.nonRetryableUpdateOneWithoutShardKeyWithIdCount);
assert.eq(1, mongosServerStatus.metrics.query.nonRetryableDeleteOneWithoutShardKeyWithIdCount);

st.stop();

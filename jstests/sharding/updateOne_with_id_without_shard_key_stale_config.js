/**
 * Tests updateOne with id without shard key works with StaleConfigError.
 *
 * @tags: [featureFlagUpdateOneWithIdWithoutShardKey, requires_fcv_73]
 */

import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({shards: 2, mongos: 2});
const mongos = st.s0;
let db = mongos.getDB(jsTestName());

const coll = db.coll;
coll.drop();

assert.commandWorked(
    db.adminCommand({enableSharding: db.getName(), primaryShard: st.rs0.getURL()}));

CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
    {min: {x: MinKey}, max: {x: -100}, shard: st.shard0.shardName},
    {min: {x: -100}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

// Write two documents.
assert.commandWorked(coll.insert({x: -1, _id: -1}));
assert.commandWorked(coll.insert({x: 1, _id: 1}));

assert.neq(st.s1.getDB(jsTestName()).coll.findOne({x: -1, _id: -1}))

// Move chunk from shard0 to shard1.
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {x: -1}, to: st.shard1.shardName}));

// This update via mongos1 should trigger a StaleConfigError as mongos1 is not aware of moved chunk.
const res = st.s1.getDB(jsTestName()).coll.updateOne({_id: -1}, {$inc: {counter: 1}});
assert.commandWorked(res);
assert.eq(res.modifiedCount, 1);
let mongosServerStatus =
    assert.commandWorked(st.s1.getDB(jsTestName()).adminCommand({serverStatus: 1}));
assert.eq(1, mongosServerStatus.metrics.query.updateOneWithoutShardKeyWithIdRetryCount);

st.stop();

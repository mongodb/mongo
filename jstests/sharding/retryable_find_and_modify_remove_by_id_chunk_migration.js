/**
 * Tests retrying a retryable findAndModify remove whose filter has an "_id" equality (and no shard
 * key) after a chunk migration has copied the session history to a second shard.
 *
 * @tags: [requires_fcv_90]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({mongos: 1, config: 1, shards: 2, rs: {nodes: 1}});

const db = st.s.getDB("test");
const collection = db.getCollection("mycoll");

// Shard on "x" (not "_id") and place chunks on both shards, so that a filter containing only an
// "_id" equality cannot be targeted to a single shard.
CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {x: 1}, [
    {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: 10}, shard: st.shard0.shardName},
    {min: {x: 10}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

assert.commandWorked(collection.insert({_id: 0, x: 5}));

// 'retryWrites: false' so that we control the txnNumber and can replay the exact same statement.
const session = st.s.startSession({causalConsistency: false, retryWrites: false});
const sessionDb = session.getDatabase(db.getName());

const findAndModifyCmd = {
    findAndModify: collection.getName(),
    query: {_id: 0},
    remove: true,
    txnNumber: NumberLong(0),
};

const firstRes = assert.commandWorked(sessionDb.runCommand(findAndModifyCmd));
jsTest.log.info("First findAndModify response", {firstRes});
assert.eq(firstRes.lastErrorObject.n, 1, firstRes);
assert.eq(firstRes.value._id, 0, firstRes);
assert.eq(collection.findOne({_id: 0}), null);

// Move the chunk that contained the removed document. This copies the session history for the
// statement above to shard1, while shard0 retains its own copy.
assert.commandWorked(
    db.adminCommand({moveChunk: collection.getFullName(), find: {x: 5}, to: st.shard1.shardName}),
);

// Retry the exact same statement. Both shards can now replay the migrated session history, so this
// is where a broadcast would produce two results for one op.
const secondRes = sessionDb.runCommand(findAndModifyCmd);
jsTest.log.info("Retried findAndModify response", {secondRes});
assert.commandWorked(secondRes);
assert.docEq(secondRes.lastErrorObject, firstRes.lastErrorObject, {secondRes, firstRes});
assert.docEq(secondRes.value, firstRes.value, {secondRes, firstRes});

// The document must not have come back and must not have been removed twice.
assert.eq(collection.find().itcount(), 0);

st.stop();

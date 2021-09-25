/**
 * Tests retryable insert, update and delete operations on a sharded collection with a nested shard
 * key to ensure that each operation is not re-executed when run after chunk migration.
 */

(function() {
"use strict";

load("jstests/sharding/libs/create_sharded_collection_util.js");

const st = new ShardingTest({mongos: 1, config: 1, shards: 2, rs: {nodes: 1}});

const db = st.s.getDB("test");
const collection = db.getCollection("mycoll");
CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {"x.y": 1}, [
    {min: {"x.y": MinKey}, max: {"x.y": 0}, shard: st.shard0.shardName},
    {min: {"x.y": 0}, max: {"x.y": 10}, shard: st.shard0.shardName},
    {min: {"x.y": 10}, max: {"x.y": 20}, shard: st.shard1.shardName},
    {min: {"x.y": 20}, max: {"x.y": MaxKey}, shard: st.shard1.shardName},
]);

assert.commandWorked(collection.insert({_id: 0, x: {y: 5}, counter: 0}));
assert.commandWorked(collection.insert({_id: 1, x: {y: 15}}));

const session = st.s.startSession({causalConsistency: false, retryWrites: false});
const sessionCollection = session.getDatabase(db.getName()).getCollection(collection.getName());

const updateCmd = {
    updates: [{q: {"x.y": 5, _id: 0}, u: {$inc: {counter: 1}}}],
    txnNumber: NumberLong(0),
};

const deleteCmd = {
    deletes: [{q: {"x.y": 15, _id: 1}, limit: 1}],
    txnNumber: NumberLong(1),
};

const insertCmd = {
    documents: [{_id: 2, x: {y: 25}}],
    txnNumber: NumberLong(2),
};

// Test that updateCmd is only executed a single time by verifying that counter has only been
// incremented once.
const firstRes = assert.commandWorked(sessionCollection.runCommand("update", updateCmd));
assert.eq({n: firstRes.n, nModified: firstRes.nModified}, {n: 1, nModified: 1});

assert.commandWorked(db.adminCommand(
    {moveChunk: collection.getFullName(), find: {"x.y": 5}, to: st.shard1.shardName}));

const secondRes = assert.commandWorked(sessionCollection.runCommand("update", updateCmd));
print(`secondRes: ${tojsononeline(secondRes)}`);
assert.eq(collection.findOne({_id: 0}), {_id: 0, x: {y: 5}, counter: 1});

// Tests deleteCmd is only executed a single time by verifying that the command is able to
// run a second time and that the response to the second command is equivalent to the first
const firstResDelete = assert.commandWorked(sessionCollection.runCommand("delete", deleteCmd));
assert.eq({n: firstResDelete.n}, {n: 1});
assert.eq(collection.findOne({_id: 1}), null);

assert.commandWorked(db.adminCommand(
    {moveChunk: collection.getFullName(), find: {"x.y": 15}, to: st.shard0.shardName}));

const secondResDelete = assert.commandWorked(sessionCollection.runCommand("delete", deleteCmd));
print(`secondResDelete: ${tojsononeline(secondResDelete)}`);
assert.eq(secondResDelete.n, firstResDelete.n);

// Tests insertCmd is only executed a single time by verifying that the command is able to
// run a second time and that the response to the second command is equivalent to the first.
//   - If command were to execute a second time, we would receieve a duplicate key error
const firstResInsert = assert.commandWorked(sessionCollection.runCommand("insert", insertCmd));
assert.eq({n: firstResInsert.n}, {n: 1});

assert.commandWorked(db.adminCommand(
    {moveChunk: collection.getFullName(), find: {"x.y": 25}, to: st.shard0.shardName}));

const secondResInsert = assert.commandWorked(sessionCollection.runCommand("insert", insertCmd));
print(`secondResInsert: ${tojsononeline(secondResInsert)}`);
assert.eq(secondResInsert.n, firstResInsert.n);
assert.eq(collection.findOne({_id: 2}), {_id: 2, x: {y: 25}});

st.stop();
})();

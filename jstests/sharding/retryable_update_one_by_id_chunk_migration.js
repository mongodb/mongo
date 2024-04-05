/**
 * This tests that mongos correctly reports 'n' and 'nModified' for retried retryable writes with
 * _id without shard key of sent with batch size of 1 after combining responses from multiple shards
 * post session migration.
 *
 * @tags: [requires_fcv_80]
 */

import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

(function() {
"use strict";

const st = new ShardingTest({mongos: 1, config: 1, shards: 2, rs: {nodes: 1}});

const db = st.s.getDB("test");
const collection = db.getCollection("mycoll");
CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {x: 1}, [
    {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: 10}, shard: st.shard0.shardName},
    {min: {x: 10}, max: {x: 20}, shard: st.shard1.shardName},
    {min: {x: 20}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

assert.commandWorked(collection.insert({_id: 0, x: 5, counter: 0}));
assert.commandWorked(collection.insert({_id: 1, x: 6, counter: 0}));

const sessionCollection = st.s.startSession({causalConsistency: false, retryWrites: false})
                              .getDatabase(db.getName())
                              .getCollection(collection.getName());

// Updates by _id are broadcasted to all shards which own chunks for the collection. After the
// session information is migrated to shard1 from the moveChunk command, both shard0 and shard1
// will report {n: 1, nModified: 1} for stmtId=0.
const updateCmd = {
    updates: [
        {q: {_id: 0}, u: {$inc: {counter: 1}}},
    ],
    txnNumber: NumberLong(0),
};

const deleteCmd = {
    deletes: [{q: {_id: 1}, limit: 1}],
    txnNumber: NumberLong(1),
}

let firstRes = assert.commandWorked(sessionCollection.runCommand("update", updateCmd));
assert.eq({n: firstRes.n, nModified: firstRes.nModified}, {n: 1, nModified: 1});

assert.commandWorked(
    db.adminCommand({moveChunk: collection.getFullName(), find: {x: 5}, to: st.shard1.shardName}));

let secondRes = assert.commandWorked(sessionCollection.runCommand("update", updateCmd));
assert.eq({n: secondRes.n, nModified: secondRes.nModified}, {n: 1, nModified: 1});

firstRes = assert.commandWorked(sessionCollection.runCommand("delete", deleteCmd));
assert.eq(firstRes.n, 1);

assert.commandWorked(
    db.adminCommand({moveChunk: collection.getFullName(), find: {x: 5}, to: st.shard0.shardName}));

secondRes = assert.commandWorked(sessionCollection.runCommand("delete", deleteCmd));
assert.eq(secondRes.n, 1);

st.stop();
})();

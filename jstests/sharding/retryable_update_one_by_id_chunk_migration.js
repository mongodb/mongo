/**
 * This tests that mongos correctly reports 'n'/'nModified' for retried retryable updates/deletes
 * with _id without shard key after combining responses from multiple shards post session migration
 * in the following cases:
 * 1) If they are sent with batch size of 1 with ordered:true or ordered:false.
 * 2) If they are sent with batch size > 1 with ordered: true.
 *
 * The case of batch size > 1 with ordered: false will be taken care by PM-3673.
 *
 * @tags: [requires_fcv_80]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

(function() {
"use strict";

const st = new ShardingTest({mongos: 1, config: 1, shards: 2, rs: {nodes: 1}});

const db = st.s.getDB("test");
const collection = db.getCollection("mycoll");
CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {x: 1}, [
    {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: 10}, shard: st.shard0.shardName},
    {min: {x: 10}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

for (let i = 0; i < 5; i++) {
    assert.commandWorked(collection.insert({_id: i, x: 5, counter: 0}));
}

const sessionCollection = st.s.startSession({causalConsistency: false, retryWrites: false})
                              .getDatabase(db.getName())
                              .getCollection(collection.getName());

// Updates by _id are broadcasted to all shards which own chunks for the collection. After the
// session information is migrated to shard1 from the moveChunk command, both shard0 and shard1
// will report {n: 1, nModified: 1} for the retried stmt ids.
const updateCmd = {
    updates: [
        {q: {_id: 0}, u: {$inc: {counter: 1}}},
    ],
    ordered: true,
    txnNumber: NumberLong(0),
};

const deleteCmd = {
    deletes: [{q: {_id: 0}, limit: 1}],
    ordered: true,
    txnNumber: NumberLong(1),
};

const updateCmdUnordered = {
    updates: [
        {q: {_id: 1}, u: {$inc: {counter: 1}}},
    ],
    ordered: false,
    txnNumber: NumberLong(2),
};

const deleteCmdUnordered = {
    deletes: [
        {q: {_id: 1}, limit: 1},
    ],
    ordered: false,
    txnNumber: NumberLong(3),
};

const updateCmdWithMultipleUpdatesOrdered = {
    updates: [
        {q: {_id: 2}, u: {$inc: {counter: 1}}},
        {q: {_id: 3}, u: {$inc: {counter: 1}}},
        {q: {_id: 4}, u: {$inc: {counter: 1}}},
    ],
    ordered: true,
    txnNumber: NumberLong(4),
};

const deleteCmdWithMultipleDeletesOrdered = {
    deletes: [
        {q: {_id: 2}, limit: 1},
        {q: {_id: 3}, limit: 1},
        {q: {_id: 4}, limit: 1},
    ],
    ordered: true,
    txnNumber: NumberLong(5),
};

function runUpdateAndMoveChunk(cmdObj, coll, toShard, expected) {
    const firstRes = assert.commandWorked(coll.runCommand("update", cmdObj));
    assert.eq({n: firstRes.n, nModified: firstRes.nModified}, expected);

    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: {x: 5}, to: toShard}));

    const secondRes = assert.commandWorked(coll.runCommand("update", cmdObj));
    assert.eq({n: secondRes.n, nModified: secondRes.nModified}, expected);
}

function runDeleteAndMoveChunk(cmdObj, coll, toShard, expected) {
    const firstRes = assert.commandWorked(coll.runCommand("delete", cmdObj));
    assert.eq(firstRes.n, expected);

    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: {x: 5}, to: toShard}));

    const secondRes = assert.commandWorked(coll.runCommand("delete", cmdObj));
    assert.eq(secondRes.n, expected);
}

runUpdateAndMoveChunk(updateCmd, sessionCollection, st.shard1.shardName, {n: 1, nModified: 1});
runDeleteAndMoveChunk(deleteCmd, sessionCollection, st.shard0.shardName, 1);

runUpdateAndMoveChunk(
    updateCmdUnordered, sessionCollection, st.shard1.shardName, {n: 1, nModified: 1});
runDeleteAndMoveChunk(deleteCmdUnordered, sessionCollection, st.shard0.shardName, 1);

runUpdateAndMoveChunk(updateCmdWithMultipleUpdatesOrdered,
                      sessionCollection,
                      st.shard1.shardName,
                      {n: 3, nModified: 3});
runDeleteAndMoveChunk(
    deleteCmdWithMultipleDeletesOrdered, sessionCollection, st.shard0.shardName, 3);

st.stop();
})();

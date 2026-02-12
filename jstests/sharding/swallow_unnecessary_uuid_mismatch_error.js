// Tests that a deleteMany operation on a cluster doesn't return a UUID mismatch in case it targets
// a shard with no chunks.
//
// @tags: [
//    requires_fcv_81,
// ]
//
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 3});

const db = st.s.getDB(jsTestName());
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

const getUUIDForCollection = function (coll) {
    return assert
        .commandWorked(db.runCommand({listCollections: 1}))
        .cursor.firstBatch.find((c) => c.name === coll.getName()).info.uuid;
};

const coll1 = db.sharded_1;
const collName1 = coll1.getName();
const collFullName1 = coll1.getFullName();

const coll2 = db.sharded_2;
const collName2 = coll2.getName();
const collFullName2 = coll2.getFullName();

assert.commandWorked(coll1.insert([{_id: 0}, {_id: 10}, {_id: 50}, {_id: 100}]));

// shard0: [inf, 50), shard1: [50, inf), shard2 has no chunks
assert.commandWorked(st.s.adminCommand({shardCollection: collFullName1, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: collFullName1, middle: {_id: 50}}));
assert.commandWorked(st.s.adminCommand({moveChunk: collFullName1, find: {_id: 50}, to: st.shard1.shardName}));

let uuid1 = getUUIDForCollection(coll1);

// Test multi deletes by _id, where the shard key equals "{_id: 1}".
let cmdObj = {
    delete: collName1,
    collectionUUID: uuid1,
    deletes: [
        {
            q: {
                $and: [{$expr: {$gte: ["$_id", {$literal: 0}]}}, {$expr: {$lt: ["$_id", {$literal: 99}]}}],
            },
            limit: 0,
            hint: {_id: 1},
        },
    ],
};

// Run a multi-write which should only target shards 0 & 1. This could target shard 2 but we
// shouldn't see any UUID mismatch errors since shard2 has no chunks and a delete is correctly
// defined in this case.
let res = db.runCommand(cmdObj);
assert.commandWorked(res);
// Only 3 documents fulfill the criteria so verify we've deleted them.
assert.eq(3, res.n);

assert.commandWorked(coll2.insert([{_id: 0}, {_id: 10}, {_id: 50}, {_id: 100}]));
assert.commandWorked(coll2.insert([{_id: 11}, {_id: 51}]));

// Create an index on {x: 1}.
assert.commandWorked(db.runCommand({createIndexes: collName2, indexes: [{name: "x_1", key: {x: 1}}]}));

// Shard the collection on {x: 1} and split up the key space so that shard0 owns [inf, 50) and
// shard1 owns [50, inf).
assert.commandWorked(st.s.adminCommand({shardCollection: collFullName2, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: collFullName2, middle: {x: 50}}));
assert.commandWorked(st.s.adminCommand({moveChunk: collFullName2, find: {x: 50}, to: st.shard1.shardName}));

let uuid2 = getUUIDForCollection(coll2);

// Test multi deletes by _id, where the shard key does not include "_id".
cmdObj = {
    delete: collName2,
    collectionUUID: uuid2,
    deletes: [
        {
            q: {
                $and: [
                    {$expr: {$eq: [{$mod: ["$_id", 2]}, {$literal: 0}]}},
                    {$expr: {$gte: ["$_id", {$literal: 0}]}},
                    {$expr: {$lt: ["$_id", {$literal: 99}]}},
                ],
            },
            limit: 0,
            hint: {_id: 1},
        },
    ],
};

res = db.runCommand(cmdObj);
assert.commandWorked(res);
assert.eq(3, res.n);

// Test non-multi deletes by _id, where the shard key does not include "_id".
cmdObj = {
    delete: collName2,
    collectionUUID: uuid2,
    deletes: [
        {q: {_id: 11}, limit: 1, hint: {_id: 1}},
        {q: {_id: 51}, limit: 1, hint: {_id: 1}},
    ],
};

res = db.runCommand(cmdObj);
assert.commandWorked(res);
assert.eq(2, res.n);

st.stop();

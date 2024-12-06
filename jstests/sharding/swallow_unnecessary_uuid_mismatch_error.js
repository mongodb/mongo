// Tests that a deleteMany operation on a cluster doesn't return a UUID mismatch in case it targets
// a shard with no chunks.
//
(function() {
"use strict";
const st = new ShardingTest({shards: 3});

const db = st.s.getDB(jsTestName());
assert.commandWorked(
    st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

const shardedColl1 = db.sharded_1;

assert.commandWorked(shardedColl1.insert({_id: 0}));
assert.commandWorked(shardedColl1.insert({_id: 10}));
assert.commandWorked(shardedColl1.insert({_id: 50}));
assert.commandWorked(shardedColl1.insert({_id: 100}));

const splitChunk = function(coll, splitPointKeyValue) {
    assert.commandWorked(
        st.s.adminCommand({split: coll.getFullName(), middle: {_id: splitPointKeyValue}}));
};

const uuid = function(coll) {
    return assert.commandWorked(db.runCommand({listCollections: 1}))
        .cursor.firstBatch.find(c => c.name === coll.getName())
        .info.uuid;
};

// shard0: [inf, 50), shard1: [50, inf), shard2 has no chunks
assert.commandWorked(
    st.s.adminCommand({shardCollection: shardedColl1.getFullName(), key: {_id: 1}}));
splitChunk(shardedColl1, 50);
assert.commandWorked(st.s.adminCommand(
    {moveChunk: shardedColl1.getFullName(), find: {_id: 50}, to: st.shard1.shardName}));

const cmdObj = {
    delete: shardedColl1.getName(),
    collectionUUID: uuid(shardedColl1),
    deletes: [{
        q: {
            $and:
                [{$expr: {$gte: ["$_id", {$literal: 0}]}}, {$expr: {$lt: ["$_id", {$literal: 99}]}}]
        },
        limit: 0,
        hint: {_id: 1}
    }],
};

// Run a multi-write which should only target shards 0 & 1. This could target shard 2 but we
// shouldn't see any UUID mismatch errors since shard2 has no chunks and a delete is correctly
// defined in this case.
let res = db.runCommand(cmdObj);
assert.commandWorked(res);
// Only 3 documents fulfill the criteria so verify we've deleted them.
assert.eq(3, res.n);
st.stop();
})();

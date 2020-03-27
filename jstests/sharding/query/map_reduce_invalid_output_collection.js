// Test that mapReduce correctly fails if the target collection is not unsharded or sharded by just
// _id.
// @tags: [requires_fcv_44]
(function() {
"use strict";

const st = new ShardingTest({shards: 2, mongos: 2});

const dbName = jsTest.name();
const nsString = dbName + ".coll";
const numDocs = 50000;
const numKeys = 1000;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: nsString, key: {key: 1}}));

// Load chunk data through the stale mongos before moving a chunk.
const staleMongos1 = st.s1;
staleMongos1.getCollection(nsString).find().itcount();

assert.commandWorked(st.s.adminCommand({split: nsString, middle: {key: numKeys / 2}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: nsString, find: {key: 0}, to: st.shard1.shardName}));

const bulk = st.s.getCollection(nsString).initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i, key: (i % numKeys), value: i % numKeys});
}
assert.commandWorked(bulk.execute());

const map = function() {
    emit(this.key, this.value);
};
const reduce = function(k, values) {
    let total = 0;
    for (let i = 0; i < values.length; i++) {
        total += values[i];
    }
    return total;
};

// Create and shard the output collection, with a shard key other than _id.
const outColl = "mr_out";
assert.commandWorked(
    st.s.adminCommand({shardCollection: dbName + "." + outColl, key: {not_id: 1}}));

// Insert a document into the output collection such that it is not dropped and recreated by the
// legacy mapReduce.
assert.commandWorked(st.s.getDB(dbName).getCollection(outColl).insert({_id: -1, not_id: 0}));

// Through the same mongos, verify that mapReduce fails since the output collection is not sharded
// by _id.
assert.commandFailedWithCode(
    st.s.getDB(dbName).runCommand(
        {mapReduce: "coll", map: map, reduce: reduce, out: {merge: outColl, sharded: true}}),
    31313);

assert.commandFailedWithCode(
    st.s.getDB(dbName).runCommand(
        {mapReduce: "coll", map: map, reduce: reduce, out: {reduce: outColl, sharded: true}}),
    31313);

// Expect a similar failure through a stale mongos.
assert.commandFailedWithCode(
    staleMongos1.getDB(dbName).runCommand(
        {mapReduce: "coll", map: map, reduce: reduce, out: {merge: outColl, sharded: true}}),
    31313);

// Mode replace is unique, since the legacy mapReduce will unconditionally drop and reshard the
// target collection on _id.
assert.commandFailedWithCode(
    st.s.getDB(dbName).runCommand(
        {mapReduce: "coll", map: map, reduce: reduce, out: {replace: outColl, sharded: true}}),
    31313);

function testAgainstValidShardedOutput(shardKey) {
    // Drop and reshard the target collection.
    st.s.getDB(dbName).getCollection(outColl).drop();
    assert.commandWorked(
        st.s.adminCommand({shardCollection: dbName + "." + outColl, key: shardKey}));

    // Insert a document into the output collection such that it is not dropped and recreated by the
    // legacy mapReduce.
    assert.commandWorked(st.s.getDB(dbName).getCollection(outColl).insert({_id: -1}));

    // Test that mapReduce succeeds since the target collection is sharded by _id.
    assert.commandWorked(st.s.getDB(dbName).runCommand(
        {mapReduce: "coll", map: map, reduce: reduce, out: {merge: outColl, sharded: true}}));

    // Run the same mapReduce through a stale mongos and expect it to pass as well.
    assert.commandWorked(st.s.getDB(dbName).getCollection(outColl).remove({}));
    assert.commandWorked(staleMongos1.getDB(dbName).runCommand(
        {mapReduce: "coll", map: map, reduce: reduce, out: {merge: outColl, sharded: true}}));
}

// Test against output collection sharded by {_id: 1}.
testAgainstValidShardedOutput({_id: 1});

// Test against output collection sharded by {_id: "hashed"}.
testAgainstValidShardedOutput({_id: "hashed"});

// Test that MR fails if the output collection is sharded by a compound key including _id.
(function testCompoundShardKey() {
    // Drop and reshard the target collection.
    st.s.getDB(dbName).getCollection(outColl).drop();
    assert.commandWorked(
        st.s.adminCommand({shardCollection: dbName + "." + outColl, key: {_id: 1, a: 1}}));

    // Insert a document into the output collection such that it is not dropped and recreated by the
    // legacy mapReduce.
    assert.commandWorked(st.s.getDB(dbName).getCollection(outColl).insert({_id: -1, a: 1}));

    // Test that mapReduce succeeds since the target collection is not sharded by only _id.
    assert.commandFailedWithCode(
        st.s.getDB(dbName).runCommand(
            {mapReduce: "coll", map: map, reduce: reduce, out: {merge: outColl, sharded: true}}),
        31313);

    // Run the same mapReduce through a stale mongos and expect it to fail as well. Make sure to
    // leave at least one document in the target collection for the same reason as above.
    assert.commandWorked(st.s.getDB(dbName).getCollection(outColl).remove({_id: {$gt: 0}}));
    assert.commandFailedWithCode(
        staleMongos1.getDB(dbName).runCommand(
            {mapReduce: "coll", map: map, reduce: reduce, out: {merge: outColl, sharded: true}}),
        31313);
})();

st.stop();
})();

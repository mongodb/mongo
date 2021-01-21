(function() {
"use strict";

load("jstests/sharding/libs/find_chunks_util.js");

var st = new ShardingTest({shards: 2});
var configDB = st.s.getDB('config');

/*
 * Test that moving database primary works after dropping a recreating the same sharded collection,
 * the new primary never owned a chunk of the sharded collection.
 */
var testDB = st.s.getDB(jsTest.name() + "_db1");
var coll = testDB['coll'];

assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));

jsTest.log("Create sharded collection with on chunk on shad 0");
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);
st.shardColl(coll, {skey: 1}, false, false);

jsTest.log("Move database primary back and forth shard 1");
st.ensurePrimaryShard(testDB.getName(), st.shard1.shardName);
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

jsTest.log("Drop sharded collection");
coll.drop();

jsTest.log("Re-Create sharded collection on shard 0");
st.shardColl(coll, {skey: 1}, false, false);

jsTest.log("Move database primary to shard 1");
st.ensurePrimaryShard(testDB.getName(), st.shard1.shardName);

jsTest.log("Drop sharded collection");
coll.drop();

/*
 * Test that moving database primary works after dropping a recreating the same sharded collection,
 * the new primary previously owned a chunk of the original collection.
 */
var testDB = st.s.getDB(jsTest.name() + "_db2");
var coll = testDB['coll'];

assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));

jsTest.log("Create sharded collection with two chunks on each shard");
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);
st.shardColl(coll, {skey: 1}, {skey: 0}, {skey: 0});

assert.eq(
    1, findChunksUtil.countChunksForNs(configDB, coll.getFullName(), {shard: st.shard0.shardName}));
assert.eq(
    1, findChunksUtil.countChunksForNs(configDB, coll.getFullName(), {shard: st.shard1.shardName}));
jsTest.log("Move all chunks to shard 0");
assert.commandWorked(st.s.adminCommand({
    moveChunk: coll.getFullName(),
    find: {skey: 10},
    to: st.shard0.shardName,
    _waitForDelete: true
}));
assert.eq(
    2, findChunksUtil.countChunksForNs(configDB, coll.getFullName(), {shard: st.shard0.shardName}));
assert.eq(
    0, findChunksUtil.countChunksForNs(configDB, coll.getFullName(), {shard: st.shard1.shardName}));

jsTest.log("Drop sharded collection");
coll.drop();

jsTest.log("Re-Create sharded collection with one chunk on shard 0");
st.shardColl(coll, {skey: 1}, false, false);
assert.eq(
    1, findChunksUtil.countChunksForNs(configDB, coll.getFullName(), {shard: st.shard0.shardName}));

jsTest.log("Move primary of DB to shard 1");
st.ensurePrimaryShard(testDB.getName(), st.shard1.shardName);

jsTest.log("Drop sharded collection");
coll.drop();

st.stop();
})();

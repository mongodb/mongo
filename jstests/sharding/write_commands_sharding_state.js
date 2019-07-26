// This test requires persistence because it assumes standalone shards will still have their data
// after restarting.
// @tags: [requires_persistence]

(function() {
'use strict';

var st = new ShardingTest({name: "write_commands", mongos: 2, shards: 2});

var dbTestName = 'WriteCommandsTestDB';
var collName = dbTestName + '.TestColl';

assert.commandWorked(st.s0.adminCommand({enablesharding: dbTestName}));
st.ensurePrimaryShard(dbTestName, st.shard0.shardName);

assert.commandWorked(st.s0.adminCommand({shardCollection: collName, key: {Key: 1}, unique: true}));

// Split at keys 10 and 20
assert.commandWorked(st.s0.adminCommand({split: collName, middle: {Key: 10}}));
assert.commandWorked(st.s0.adminCommand({split: collName, middle: {Key: 20}}));

printjson(st.config.getSiblingDB('config').chunks.find().toArray());

// Move 10 and 20 to st.shard0.shardName1
assert.commandWorked(st.s0.adminCommand(
    {moveChunk: collName, find: {Key: 19}, to: st.shard1.shardName, _waitForDelete: true}));
assert.commandWorked(st.s0.adminCommand(
    {moveChunk: collName, find: {Key: 21}, to: st.shard1.shardName, _waitForDelete: true}));

printjson(st.config.getSiblingDB('config').chunks.find().toArray());

// Insert one document in each chunk, which we will use to change
assert(st.s1.getDB(dbTestName).TestColl.insert({Key: 1}));
assert(st.s1.getDB(dbTestName).TestColl.insert({Key: 11}));
assert(st.s1.getDB(dbTestName).TestColl.insert({Key: 21}));

// Make sure the documents are correctly placed
printjson(st.shard0.getDB(dbTestName).TestColl.find().toArray());
printjson(st.shard1.getDB(dbTestName).TestColl.find().toArray());

assert.eq(1, st.shard0.getDB(dbTestName).TestColl.count());
assert.eq(2, st.shard1.getDB(dbTestName).TestColl.count());

assert.eq(1, st.shard0.getDB(dbTestName).TestColl.find({Key: 1}).count());
assert.eq(1, st.shard1.getDB(dbTestName).TestColl.find({Key: 11}).count());
assert.eq(1, st.shard1.getDB(dbTestName).TestColl.find({Key: 21}).count());

// Move chunk [0, 19] to st.shard0.shardName and make sure the documents are correctly placed
assert.commandWorked(st.s0.adminCommand(
    {moveChunk: collName, find: {Key: 19}, _waitForDelete: true, to: st.shard0.shardName}));

printjson(st.config.getSiblingDB('config').chunks.find().toArray());
printjson(st.shard0.getDB(dbTestName).TestColl.find({}).toArray());
printjson(st.shard1.getDB(dbTestName).TestColl.find({}).toArray());

// Now restart all mongod instances, so they don't know yet that they are sharded
st.restartShardRS(0);
st.restartShardRS(1);

// Now that both mongod shards are restarted, they don't know yet that they are part of a
// sharded
// cluster until they get a setShardVerion command. Mongos instance s1 has stale metadata and
// doesn't know that chunk with key 19 has moved to st.shard0.shardName so it will send it to
// st.shard1.shardName at
// first.
//
// Shard0001 would only send back a stale config exception if it receives a setShardVersion
// command. The bug that this test validates is that setShardVersion is indeed being sent (for
// more
// information, see SERVER-19395).
st.s1.getDB(dbTestName).TestColl.update({Key: 11}, {$inc: {Counter: 1}}, {upsert: true});

printjson(st.shard0.getDB(dbTestName).TestColl.find({}).toArray());
printjson(st.shard1.getDB(dbTestName).TestColl.find({}).toArray());

assert.eq(2, st.shard0.getDB(dbTestName).TestColl.count());
assert.eq(1, st.shard1.getDB(dbTestName).TestColl.count());

assert.eq(1, st.shard0.getDB(dbTestName).TestColl.find({Key: 1}).count());
assert.eq(1, st.shard0.getDB(dbTestName).TestColl.find({Key: 11}).count());
assert.eq(1, st.shard1.getDB(dbTestName).TestColl.find({Key: 21}).count());

st.stop();
})();

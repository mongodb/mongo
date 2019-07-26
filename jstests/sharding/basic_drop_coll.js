/**
 * Basic test from the drop collection command on a sharded cluster that verifies collections are
 * cleaned up properly.
 */
(function() {
"use strict";

var st = new ShardingTest({shards: 2});

var testDB = st.s.getDB('test');

// Test dropping an unsharded collection.

assert.writeOK(testDB.bar.insert({x: 1}));
assert.neq(null, testDB.bar.findOne({x: 1}));

assert.commandWorked(testDB.runCommand({drop: 'bar'}));
assert.eq(null, testDB.bar.findOne({x: 1}));

// Test dropping a sharded collection.

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);
st.s.adminCommand({shardCollection: 'test.user', key: {_id: 1}});
st.s.adminCommand({split: 'test.user', middle: {_id: 0}});
assert.commandWorked(
    st.s.adminCommand({moveChunk: 'test.user', find: {_id: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: 'foo'}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: 'test.user', min: {_id: 0}, max: {_id: 10}, zone: 'foo'}));

assert.writeOK(testDB.user.insert({_id: 10}));
assert.writeOK(testDB.user.insert({_id: -10}));

assert.neq(null, st.shard0.getDB('test').user.findOne({_id: -10}));
assert.neq(null, st.shard1.getDB('test').user.findOne({_id: 10}));

var configDB = st.s.getDB('config');
var collDoc = configDB.collections.findOne({_id: 'test.user'});

assert(!collDoc.dropped);

assert.eq(2, configDB.chunks.count({ns: 'test.user'}));
assert.eq(1, configDB.tags.count({ns: 'test.user'}));

assert.commandWorked(testDB.runCommand({drop: 'user'}));

assert.eq(null, st.shard0.getDB('test').user.findOne());
assert.eq(null, st.shard1.getDB('test').user.findOne());

// Call drop again to verify that the command is idempotent.
assert.commandWorked(testDB.runCommand({drop: 'user'}));

// Check for the collection with majority RC to verify that the write to remove the collection
// document from the catalog has propagated to the majority snapshot.
var findColl = configDB.runCommand(
    {find: 'collections', filter: {_id: 'test.user'}, readConcern: {'level': 'majority'}});
collDoc = findColl.cursor.firstBatch[0];

assert(collDoc.dropped);

assert.eq(0, configDB.chunks.count({ns: 'test.user'}));
assert.eq(0, configDB.tags.count({ns: 'test.user'}));

st.stop();
})();

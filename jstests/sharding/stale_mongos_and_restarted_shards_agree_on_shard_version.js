/**
 * Tests that after a restart on a shard multi write operations, finds and aggregations
 * work as expected on stale routers
 *
 * This test requrires persistence because it asumes the shard will still have it's
 * data after restarting
 * TODO: Remove requires_fcv_44 tag if SERVER-32198 is backported or 4.4 becomes last
 * @tags: [requires_persistence, requires_fcv_44]
 */
(function() {
'use strict';
// TODO (SERVER-32198) remove after SERVER-32198 is fixed
TestData.skipCheckOrphans = true;

const st = new ShardingTest({shards: 2, mongos: 2});

// Used to get the shard destination ids for the moveChunks commands
const shard0Name = st.shard0.shardName;
const shard1Name = st.shard1.shardName;

var database = 'TestDB';
st.enableSharding(database);
// Creates and shard collName with 2 chunks, one per shard. Only one router knows that collName
// is sharded, and all the shards are restarted so they don't have the collection's sharding status
function setupCollectionForTest(collName) {
    var ns = database + '.' + collName;
    assert.commandFailedWithCode(st.s0.adminCommand({getShardVersion: ns}),
                                 ErrorCodes.NamespaceNotSharded);
    assert.commandFailedWithCode(st.s1.adminCommand({getShardVersion: ns}),
                                 ErrorCodes.NamespaceNotSharded);
    st.shardCollection(ns, {Key: 1});

    st.s0.adminCommand({split: ns, middle: {Key: 0}});
    st.s0.adminCommand({moveChunk: ns, find: {Key: -1}, to: shard0Name});
    st.s0.adminCommand({moveChunk: ns, find: {Key: 0}, to: shard1Name});
    assert.commandFailedWithCode(st.s1.adminCommand({getShardVersion: ns}),
                                 ErrorCodes.NamespaceNotSharded);

    // This document will go to shard 0
    assert.commandWorked(st.s0.getDB(database).getCollection(collName).insert({Key: -1, inc: 0}));
    // This document will go to shard 1
    assert.commandWorked(st.s0.getDB(database).getCollection(collName).insert({Key: 0, inc: 0}));
    st.restartShardRS(0);
    st.restartShardRS(1);
}

// Test a multi insert with collection unknown on a stale mongos
setupCollectionForTest('TestColl');

var bulkOp = st.s1.getDB('TestDB').TestColl.initializeUnorderedBulkOp();
bulkOp.insert({Key: -2});
bulkOp.insert({Key: 1});
bulkOp.execute();

assert.neq(4, st.s0.getDB('TestDB').TestColl.find().itcount());
// TODO (SERVER-32198): After SERVER-32198 is fixed and backported change neq to eq
assert.neq(4, st.s1.getDB('TestDB').TestColl.find().itcount());

// Test multi update with collection unknown on a stale mongos
setupCollectionForTest('TestUpdateColl');

var updateBulkOp = st.s1.getDB('TestDB').TestUpdateColl.initializeUnorderedBulkOp();
updateBulkOp.find({}).update({$inc: {inc: 1}});
updateBulkOp.execute();
var s0Doc = st.s0.getDB('TestDB').TestUpdateColl.findOne({Key: -1});
// TODO (SERVER-32198): After SERVER-32198 is fixed and backported change neq to eq
assert.neq(1, s0Doc.inc);
var s1Doc = st.s0.getDB('TestDB').TestUpdateColl.findOne({Key: 0});
assert.eq(1, s1Doc.inc);

// Test multi remove with collection unknown on a stale mongos
setupCollectionForTest('TestRemoveColl');

var removeBulkOp = st.s1.getDB('TestDB').TestRemoveColl.initializeUnorderedBulkOp();
removeBulkOp.find({}).remove({});
removeBulkOp.execute();
// TODO (SERVER-32198): After SERVER-32198 is fixed and backported change neq to eq
assert.neq(0, st.s0.getDB('TestDB').TestRemoveColl.find().itcount());

// Test find with collection unknown on a stale mongos
setupCollectionForTest('TestFindColl');

var coll = st.s1.getDB('TestDB').TestFindColl.find().toArray();
// TODO (SERVER-32198): After SERVER-32198 is fixed and backported change neq to eq
assert.neq(2, coll.length);

// Test aggregate with collection unknown on a stale mongos
setupCollectionForTest('TestAggregateColl');

var count = st.s1.getDB('TestDB').TestAggregateColl.aggregate([{$count: 'total'}]).toArray();
// TODO (SERVER-32198): After SERVER-32198 is fixed and backported change neq to eq
assert.neq(2, count[0].total);

// Test transactions with unsharded collection, which is unknown on the shard
st.restartShardRS(0);
st.restartShardRS(1);

var session = st.s1.startSession();
session.startTransaction();
session.getDatabase('TestDB').getCollection('TestTransactionColl').insert({Key: 1});
session.commitTransaction();

st.stop();
})();

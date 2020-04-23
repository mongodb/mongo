/**
 * Verify that a recipient shard drops indexes that do not exist on the donor shard before
 * receiving its first chunk.
 */
(function() {
"use strict";
const st = new ShardingTest({shards: 2});
const dbName = 'test';
const testDB = st.s.getDB('test');
const collName = jsTest.name();
const coll = testDB[collName];
const nDocs = 100;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: nDocs / 2}}));

var bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    bulk.insert({_id: i, a: i, b: i * 2});
}
assert.commandWorked(bulk.execute());

let shard0DB = st.shard0.getDB(dbName);
let shard1DB = st.shard1.getDB(dbName);
let shard0Coll = shard0DB[collName];
let shard1Coll = shard1DB[collName];

// shard0 should have all of the documents; shard1 should have none.
assert.eq(nDocs, shard0Coll.find().itcount());
assert.eq(0, shard1Coll.find().itcount());

// Move first chunk to and from shard1 to guarantee that the collection is created locally.
assert.commandWorked(st.s.adminCommand(
    {moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName, waitForDelete: true}));

assert.commandWorked(st.s.adminCommand(
    {moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard0.shardName, waitForDelete: true}));

// Manually create an index on shard1.
const indexName = "shardSpecficIndex";
assert.commandWorked(shard1Coll.createIndex({a: 1, b: -1}, {name: indexName}));

// Verify index is on shard1 but not shard0.
assert.eq(0,
          shard0Coll.aggregate([{$indexStats: {}}, {$match: {name: indexName}}]).toArray().length);
assert.eq(1,
          shard1Coll.aggregate([{$indexStats: {}}, {$match: {name: indexName}}]).toArray().length);

// Move chunk back to shard1 and verify that local index is no longer present.
assert.commandWorked(st.s.adminCommand(
    {moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName, waitForDelete: true}));

assert.eq(0,
          shard1Coll.aggregate([{$indexStats: {}}, {$match: {name: indexName}}]).toArray().length);

// Verify that we didn't drop _id index.
assert.eq(1, shard1Coll.aggregate([{$indexStats: {}}, {$match: {name: "_id_"}}]).toArray().length);

// Verify that when two shards have an index with the same name and key but different options,
// the recipient shard will drop its local index.

// Move chunk back to shard0 to allow for shard1 to receive first chunk again.
assert.commandWorked(st.s.adminCommand(
    {moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard0.shardName, waitForDelete: true}));

const commonIndex = {
    a: -1
};

const commonIndexName = "commonIndex";

// Create index on both shards
assert.commandWorked(
    shard0Coll.createIndex(commonIndex, {name: commonIndexName, expireAfterSeconds: 2000}));
assert.commandWorked(
    shard1Coll.createIndex(commonIndex, {name: commonIndexName, expireAfterSeconds: 2000}));

// Now modify the index on shard1 to differ by one option.
assert.commandWorked(shard1DB.runCommand(
    {collMod: collName, index: {keyPattern: commonIndex, expireAfterSeconds: 5000}}));

// Verify that the indexes are not equal.
// Note that we only compare the name and spec fields because extra information from $indexStats
// such as "accesses" could break the comparison of two indexes.
const commonIndexPipeline =
    [{$indexStats: {}}, {$match: {name: commonIndexName}}, {$project: {name: 1, spec: 1}}];

let shard0Index = shard0Coll.aggregate(commonIndexPipeline).toArray();
let shard1Index = shard1Coll.aggregate(commonIndexPipeline).toArray();
assert.neq(shard0Index, shard1Index);

// Now perform migration and verify that indexes are equal.
assert.commandWorked(st.s.adminCommand(
    {moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName, waitForDelete: true}));
shard0Index = shard0Coll.aggregate(commonIndexPipeline).toArray();
shard1Index = shard1Coll.aggregate(commonIndexPipeline).toArray();
assert.eq(shard0Index, shard1Index);

st.stop();
})();

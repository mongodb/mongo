/*
 * Test that crud and find operations target the right shards during migration.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
'use strict';

load('jstests/libs/chunk_manipulation_util.js');
load("jstests/sharding/libs/chunk_bounds_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

let st = new ShardingTest({shards: 3});
let dbName = "test";
let collName = "user";
let ns = dbName + "." + collName;
let configDB = st.s.getDB('config');
let testDB = st.s.getDB(dbName);

// For startParallelOps to write its state.
let staticMongod = MongoRunner.runMongod({});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard1.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 'hashed'}}));

let chunkDocs = findChunksUtil.findChunksByNs(configDB, ns).toArray();
let shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunkDocs);

jsTest.log("Test 'insert'");
// Insert a doc while migrating the chunk that the doc belongs to.
let doc = {x: 0};
let hash = convertShardKeyToHashed(doc.x);
let shardBoundsPair =
    chunkBoundsUtil.findShardAndChunkBoundsForShardKey(st, shardChunkBounds, {x: hash});
let fromShard = shardBoundsPair.shard;
let toShard = st.getOther(fromShard);
runCommandDuringTransferMods(
    st.s, staticMongod, ns, shardBoundsPair.bounds, fromShard, toShard, () => {
        assert.commandWorked(testDB.user.insert(doc));
    });

// Check that the inserted doc is on the recipient shard.
assert.eq(1, testDB.user.find(doc).count());
assert.eq(1, toShard.getCollection(ns).find(doc).count());

// Clean up.
assert.commandWorked(testDB.user.remove({}));
chunkDocs = findChunksUtil.findChunksByNs(configDB, ns).toArray();
shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunkDocs);

// Insert docs that are expected to go to three different shards, check that the docs
// are on the right shards and store the shard and chunk bounds for each doc.
let docs = [{x: -10}, {x: -1}, {x: 10}];
assert.commandWorked(testDB.user.insert(docs));
let shards = [];
let docChunkBounds = [];
for (let doc of docs) {
    let hash = convertShardKeyToHashed(doc.x);
    let shardBoundsPair =
        chunkBoundsUtil.findShardAndChunkBoundsForShardKey(st, shardChunkBounds, {x: hash});
    assert.eq(1, shardBoundsPair.shard.getCollection(ns).find(doc).count());
    shards.push(shardBoundsPair.shard);
    docChunkBounds.push(shardBoundsPair.bounds);
}
assert.eq(3, (new Set(shards)).size);
assert.eq(3, testDB.user.find({}).count());

// Perform a series of operations on docs[1] while moving the chunk that it belongs to
// from shards[1] to shards[2], then to shards[0] and back to shards[1].

jsTest.log("Test 'update'");
// Update the doc while migrating the chunk.
fromShard = shards[1];
toShard = shards[2];
runCommandDuringTransferMods(st.s, staticMongod, ns, docChunkBounds[1], fromShard, toShard, () => {
    assert.commandWorked(testDB.user.update({x: -1}, {$set: {updated: true}}, {multi: true}));
});

// Check that the doc is updated correctly.
assert.eq(1, testDB.user.find({x: -1, updated: true}).count());
assert.eq(0, shards[0].getCollection(ns).find({updated: true}).count());
assert.eq(0, shards[1].getCollection(ns).find({updated: true}).count());
assert.eq(1, shards[2].getCollection(ns).find({updated: true}).count());

jsTest.log("Test 'findAndModify'");
// findAndModify the doc while migrating the chunk.
fromShard = shards[2];
toShard = shards[0];
runCommandDuringTransferMods(st.s, staticMongod, ns, docChunkBounds[1], fromShard, toShard, () => {
    assert.commandWorked(
        testDB.runCommand({findAndModify: collName, query: {x: -1}, update: {$set: {y: 1}}}));
});

// Check that the doc is updated correctly.
assert.eq(1, testDB.user.find({x: -1, y: 1}).count());
assert.eq(1, shards[0].getCollection(ns).count({y: 1}));
assert.eq(0, shards[1].getCollection(ns).count({y: 1}));
assert.eq(0, shards[2].getCollection(ns).count({y: 1}));

jsTest.log("Test 'remove'");
// Remove the doc while migrating the chunk.
fromShard = shards[0];
toShard = shards[1];
runCommandDuringTransferMods(st.s, staticMongod, ns, docChunkBounds[1], fromShard, toShard, () => {
    assert.commandWorked(testDB.user.remove({x: -1}));
});

// Check that the doc is removed correctly.
assert.eq(2, testDB.user.find({}).count());
assert.eq(1, shards[0].getCollection(ns).find({}).count());
assert.eq(0, shards[1].getCollection(ns).find({}).count());
assert.eq(1, shards[2].getCollection(ns).find({}).count());

st.stop();
MongoRunner.stopMongod(staticMongod);
})();

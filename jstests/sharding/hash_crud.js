/*
 * Test that crud and find operations target the right shards.
 */
(function() {
'use strict';

load("jstests/sharding/libs/chunk_bounds_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

let st = new ShardingTest({shards: 3});
let dbName = "test";
let collName = "user";
let ns = dbName + "." + collName;
let configDB = st.s.getDB('config');
let testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard1.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 'hashed'}}));

let chunkDocs = findChunksUtil.findChunksByNs(configDB, ns).toArray();
let shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunkDocs);

jsTest.log("Test 'insert'");
// Insert docs that are expected to go to three different shards.
let docs = [{x: -10}, {x: -1}, {x: 10}];
assert.commandWorked(testDB.user.insert(docs));

// Check that the docs are on the right shards and store the shard for each doc.
let shards = [];
for (let doc of docs) {
    let hash = convertShardKeyToHashed(doc.x);
    let shard = chunkBoundsUtil.findShardForShardKey(st, shardChunkBounds, {x: hash});
    assert.eq(1, shard.getCollection(ns).count(doc));
    shards.push(shard);
}
assert.eq(3, (new Set(shards)).size);

jsTest.log("Test 'find'");
assert.eq(3, testDB.user.find({}).count());
assert.eq(2, testDB.user.find({x: {$lt: 0}}).count());

jsTest.log("Test 'update'");
assert.commandWorked(testDB.user.update({x: -10}, {$set: {updated: true}}, {multi: true}));
assert.eq(1, testDB.user.find({x: -10, updated: true}).count());
assert.eq(1, shards[0].getCollection(ns).count({updated: true}));
assert.eq(0, shards[1].getCollection(ns).count({updated: true}));
assert.eq(0, shards[2].getCollection(ns).count({updated: true}));

jsTest.log("Test 'findAndModify'");
assert.commandWorked(
    testDB.runCommand({findAndModify: collName, query: {x: -1}, update: {$set: {y: 1}}}));
assert.eq(1, testDB.user.find({x: -1, y: 1}).count());
assert.eq(0, shards[0].getCollection(ns).count({y: 1}));
assert.eq(1, shards[1].getCollection(ns).count({y: 1}));
assert.eq(0, shards[2].getCollection(ns).count({y: 1}));

jsTest.log("Test 'remove'");
assert.commandWorked(testDB.user.remove({x: 10}));
assert.eq(2, testDB.user.find({}).count());
assert.eq(1, shards[0].getCollection(ns).count({}));
assert.eq(1, shards[1].getCollection(ns).count({}));
assert.eq(0, shards[2].getCollection(ns).count({}));

st.stop();
})();

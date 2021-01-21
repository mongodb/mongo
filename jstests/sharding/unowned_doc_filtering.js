/**
 * Tests that the mongod chunk filtering stage properly filters out unowned documents even after
 * the shards are restarted.
 *
 * This test involves restarting a standalone shard, so cannot be run on ephemeral storage engines.
 * A restarted standalone will lose all data when using an ephemeral storage engine.
 * @tags: [requires_persistence]
 */

// This test shuts down shards.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Test deliberately inserts orphans.
TestData.skipCheckOrphans = true;

(function() {
"use strict";

load("jstests/sharding/libs/chunk_bounds_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

/*
 * Asserts that find and count command filter out unowned documents.
 */
function assertOrphanedDocsFiltered(coll, ownedDocs, unownedDocs, countFilters) {
    assert.eq(ownedDocs.length, coll.find().itcount());
    for (let doc of unownedDocs) {
        assert.eq(0, coll.find(doc).itcount());
        assert.eq(0, coll.count(doc));
    }
    for (let {filter, count} of countFilters) {
        assert.eq(count, coll.find(filter).itcount());
        assert.eq(count, coll.count(filter));
    }
}

function runTest(st, coll, ownedDocs, unownedDocs, isHashed) {
    let ns = coll.getFullName();
    let chunkDocs = findChunksUtil.findChunksByNs(st.s.getDB('config'), ns).toArray();
    let shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunkDocs);

    // Do regular inserts.
    assert.commandWorked(coll.insert(ownedDocs));
    assert.eq(ownedDocs.length, coll.find().itcount());

    // Create unowned docs by inserting the docs directly into the shards that do not
    // own the chunks for the docs.
    for (let doc of unownedDocs) {
        let shardKey = {x: isHashed ? convertShardKeyToHashed(doc.x) : doc.x};
        let shardWithChunk = chunkBoundsUtil.findShardForShardKey(st, shardChunkBounds, shardKey);
        let shardToInsert = st.getOther(shardWithChunk);
        assert.commandWorked(shardToInsert.getCollection(ns).insert(doc));
    }

    // Check that unowned docs are filtered correctly.
    let countFilters = [
        {filter: {x: {$lte: 0}}, count: ownedDocs.filter(doc => doc.x <= 0).length},
        {filter: {x: {$gt: 0}}, count: ownedDocs.filter(doc => doc.x > 0).length}
    ];
    assertOrphanedDocsFiltered(coll, ownedDocs, unownedDocs, countFilters);

    // Restart the shards, wait for them to become available and redo the check.
    st.restartShardRS(0, undefined, undefined, true);
    st.restartShardRS(1, undefined, undefined, true);
    assertOrphanedDocsFiltered(coll, ownedDocs, unownedDocs, countFilters);
}

let st = new ShardingTest({shards: 2});
let dbName = "test";
let testDB = st.s.getDB(dbName);
let rangeShardedColl = testDB.range;
let hashedShardedColl = testDB.hashed;
let rangeShardedNs = rangeShardedColl.getFullName();
let hashedShardedNs = hashedShardedColl.getFullName();

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

jsTest.log("Test range sharding...");
assert.commandWorked(testDB.adminCommand({shardCollection: rangeShardedNs, key: {x: 1}}));
assert.commandWorked(testDB.adminCommand({split: rangeShardedNs, middle: {x: 50}}));
assert.commandWorked(
    testDB.adminCommand({moveChunk: rangeShardedNs, find: {x: 100}, to: st.shard1.shardName}));

let ownedDocs = [];
for (let i = 0; i < 100; i++) {
    ownedDocs.push({x: i});
}
let unownedDocs = [{x: 100}, {x: -1}];
runTest(st, rangeShardedColl, ownedDocs, unownedDocs, false);

jsTest.log("Test hashed sharding...");
assert.commandWorked(st.s.adminCommand({shardCollection: hashedShardedNs, key: {x: 'hashed'}}));

ownedDocs = [{x: -1000}, {x: 0}, {x: 5}];
unownedDocs = [{x: -5}, {x: 10}];
runTest(st, hashedShardedColl, ownedDocs, unownedDocs, true);

st.stop();
}());

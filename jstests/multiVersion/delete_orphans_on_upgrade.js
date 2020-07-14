/*
 *  Tests that orphans eventually get deleted after setting the featureCompatibilityVersion to 4.4.
 */

(function() {
"use strict";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Create 2 shards with 3 replicas each.
let st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

jsTestLog("Setting FCV: " + lastLTSFCV);
assert.commandWorked(st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
checkFCV(st.shard0.getDB("admin"), lastLTSFCV);

// Create a sharded collection with four chunks: [-inf, 50), [50, 100), [100, 150) [150, inf)
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 100}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 150}}));

// Move chunks [50, 100) and [150, inf) to shard1 to create a gap.
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {x: 50}, to: st.shard1.shardName, _waitForDelete: true}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {x: 150}, to: st.shard1.shardName, _waitForDelete: true}));

let testDB = st.s.getDB(dbName);
let testColl = testDB.foo;

// Insert documents into each chunk
for (let i = 0; i < 200; i++) {
    testColl.insert({x: i});
}

const expectedNumDocsTotal = 200;
const expectedNumDocsShard0 = 100;
const expectedNumDocsShard1 = 100;

// Verify total count.
assert.eq(testColl.find().itcount(), expectedNumDocsTotal);

// Verify shard0 count.
let shard0Coll = st.shard0.getCollection(ns);
assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0);

// Verify shard1 count.
let shard1Coll = st.shard1.getCollection(ns);
assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

// Write some orphaned documents directly to shard0 that are on two separate, discontiguous chunks.
let orphanCount = 0;
for (let i = 70; i < 90; i++) {
    shard0Coll.insert({x: i});
    ++orphanCount;
}
for (let i = 150; i < 170; i++) {
    shard0Coll.insert({x: i});
    ++orphanCount;
}

// Verify counts.
assert.eq(testColl.find().itcount(), expectedNumDocsTotal);
assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0 + orphanCount);
assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

jsTestLog("Setting FCV: " + latestFCV);
assert.commandWorked(st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.shard0.getDB("admin"), latestFCV);

// Verify counts.
assert.eq(testColl.find().itcount(), expectedNumDocsTotal);
assert.soon(() => shard0Coll.find().itcount() == expectedNumDocsShard0);
assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

st.stop();
})();

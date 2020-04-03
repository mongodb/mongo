/**
 * Ensure that orphaned documents are deleted when the pending = true field is removed from the
 * config.rangeDeletions collection.
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Create 2 shards with 3 replicas each.
let st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

// Create a sharded collection with two chunks: [-inf, 50), [50, inf)
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

// Move chunk [50, inf) to shard1.
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {x: 50}, to: st.shard1.shardName, _waitForDelete: true}));

let testDB = st.s.getDB(dbName);
let testColl = testDB.foo;

(() => {
    // Insert documents into each chunk
    jsTestLog("Inserting documents");
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    const expectedNumDocsTotal = 100;
    const expectedNumDocsShard0 = 50;
    const expectedNumDocsShard1 = 50;

    jsTestLog("Verifying counts");

    // Verify total count.
    assert.eq(testColl.find().itcount(), expectedNumDocsTotal);

    // Verify shard0 count.
    let shard0Coll = st.shard0.getCollection(ns);
    assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0);

    // Verify shard1 count.
    let shard1Coll = st.shard1.getCollection(ns);
    assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

    // Write some orphaned documents directly to shard0.
    jsTestLog("Inserting orphans");
    let orphanCount = 0;
    for (let i = 70; i < 90; i++) {
        assert.commandWorked(shard0Coll.insert({x: i}));
        ++orphanCount;
    }

    // Verify counts.
    jsTestLog("Verifying counts with orphans");
    assert.eq(testColl.find().itcount(), expectedNumDocsTotal);
    assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0 + orphanCount);
    assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

    const collectionUuid = getUUIDFromConfigCollections(st.s, ns);

    let deletionTask = {
        _id: UUID(),
        nss: ns,
        collectionUuid: collectionUuid,
        donorShardId: "unused",
        pending: true,
        range: {min: {x: 70}, max: {x: 90}},
        whenToClean: "now"
    };

    const rangeDeletionNs = "config.rangeDeletions";
    let deletionsColl = st.shard0.getCollection(rangeDeletionNs);

    // Write range to deletion collection
    jsTestLog("Inserting deletion task");
    deletionsColl.insert(deletionTask);

    // Update deletion task
    jsTestLog("Updating pending flag");
    deletionsColl.update(deletionTask, {$unset: {pending: ""}});

    // Verify that orphans are deleted.
    assert.soon(() => {
        return shard0Coll.find().itcount() == expectedNumDocsShard0;
    });

    assert.commandWorked(testColl.deleteMany({}));
    assert.commandWorked(deletionsColl.deleteMany({}));
})();

// Test failure to match collection uuid.
(() => {
    // Insert documents into each chunk
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    const expectedNumDocsTotal = 100;
    const expectedNumDocsShard0 = 50;
    const expectedNumDocsShard1 = 50;

    // Verify total count.
    assert.eq(testColl.find().itcount(), expectedNumDocsTotal);

    // Verify shard0 count.
    let shard0Coll = st.shard0.getCollection(ns);
    assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0);

    // Verify shard1 count.
    let shard1Coll = st.shard1.getCollection(ns);
    assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

    let deletionTask = {
        _id: UUID(),
        nss: ns,
        collectionUuid: UUID(),
        donorShardId: "unused",
        pending: true,
        range: {min: {x: 70}, max: {x: 90}},
        whenToClean: "now"
    };

    const rangeDeletionNs = "config.rangeDeletions";
    let deletionsColl = st.shard0.getCollection(rangeDeletionNs);

    // Write range to deletion collection
    deletionsColl.insert(deletionTask);

    // Update deletion task
    deletionsColl.update(deletionTask, {$unset: {pending: ""}});

    // Verify that the deletion task gets deleted after being processed.
    assert.soon(function() {
        return deletionsColl.find().itcount() === 0;
    });

    // Verify counts on shards are correct.
    assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0);
    assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

    assert.commandWorked(testColl.deleteMany({}));
    assert.commandWorked(deletionsColl.deleteMany({}));
})();

st.stop();
})();

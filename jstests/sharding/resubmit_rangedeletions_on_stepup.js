/**
 * Ensure that orphaned documents are submitted for deletion on step up.
 * @tags: [multiversion_incompatible]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const rangeDeletionNs = "config.rangeDeletions";

function setup() {
    // Create 2 shards with 3 replicas each.
    let st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

    // Create a sharded collection with two chunks: [-inf, 50), [50, inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

    return st;
}

// Test normal case where the pending field has been removed and the orphans are deleted.
(() => {
    let st = setup();

    let testDB = st.s.getDB(dbName);
    let testColl = testDB.foo;

    // Insert documents into first chunk
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    // Pause range deletion.
    let originalShard0Primary = st.rs0.getPrimary();
    originalShard0Primary.adminCommand(
        {configureFailPoint: 'suspendRangeDeletion', mode: 'alwaysOn'});

    // Move chunk [50, inf) to shard1.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    const collectionUuid = getUUIDFromConfigCollections(st.s, ns);

    let deletionTask = {
        nss: ns,
        collectionUuid: collectionUuid,
        range: {min: {x: 50}, max: {x: MaxKey}},
        whenToClean: "now"
    };

    let deletionsColl = st.shard0.getCollection(rangeDeletionNs);

    // Write range to deletion collection
    deletionsColl.insert(deletionTask);

    const expectedNumDocsTotal = 100;
    const expectedNumDocsShard0 = 50;
    const expectedNumDocsShard1 = 50;

    // Verify total count.
    assert.eq(testColl.find().itcount(), expectedNumDocsTotal);

    // Verify shard0 count includes orphans.
    let shard0Coll = st.shard0.getCollection(ns);
    assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0 + expectedNumDocsShard1);

    // Verify shard1 count.
    let shard1Coll = st.shard1.getCollection(ns);
    assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

    // Step down current primary.
    assert.commandWorked(originalShard0Primary.adminCommand({replSetStepDown: 60, force: 1}));

    // Connect to new primary for shard0.
    let shard0Primary = st.rs0.getPrimary();
    let shard0PrimaryColl = shard0Primary.getCollection(ns);

    // Verify that orphans are deleted.
    assert.soon(() => {
        return shard0PrimaryColl.find().itcount() == expectedNumDocsShard0;
    });

    st.stop();
})();

// Test the case where pending: true and the orphans are NOT deleted.
(() => {
    let st = setup();

    let testDB = st.s.getDB(dbName);
    let testColl = testDB.foo;

    // Insert documents into first chunk
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    // Pause range deletion.
    let originalShard0Primary = st.rs0.getPrimary();
    originalShard0Primary.adminCommand(
        {configureFailPoint: 'suspendRangeDeletion', mode: 'alwaysOn'});

    // Move chunk [50, inf) to shard1.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    const collectionUuid = getUUIDFromConfigCollections(st.s, ns);

    let deletionTask = {
        nss: ns,
        collectionUuid: collectionUuid,
        pending: true,
        range: {min: {x: 50}, max: {x: MaxKey}},
        whenToClean: "now"
    };

    let deletionsColl = st.shard0.getCollection(rangeDeletionNs);

    // Write range to deletion collection
    deletionsColl.insert(deletionTask);

    const expectedNumDocsTotal = 100;
    const expectedNumDocsShard0 = 50;
    const expectedNumDocsShard1 = 50;

    // Verify total count.
    assert.eq(testColl.find().itcount(), expectedNumDocsTotal);

    // Verify shard0 count includes orphans.
    let shard0Coll = st.shard0.getCollection(ns);
    assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0 + expectedNumDocsShard1);

    // Verify shard1 count.
    let shard1Coll = st.shard1.getCollection(ns);
    assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

    // Step down current primary.
    assert.commandWorked(originalShard0Primary.adminCommand({replSetStepDown: 60, force: 1}));

    // Connect to new primary for shard0.
    let shard0Primary = st.rs0.getPrimary();
    let shard0PrimaryColl = shard0Primary.getCollection(ns);

    // Verify that orphans are NOT deleted.
    assert.eq(shard0PrimaryColl.find().itcount(), expectedNumDocsShard0 + expectedNumDocsShard1);

    st.stop();
})();
})();

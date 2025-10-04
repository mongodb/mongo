/**
 * Ensure that orphaned documents are submitted for deletion on step up.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getUUIDFromConfigCollections} from "jstests/libs/uuid_util.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
// Test deliberately keeps range deletion in pending state.
TestData.skipCheckOrphans = true;

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const rangeDeletionNs = "config.rangeDeletions";

function setup() {
    // Create 2 shards with 3 replicas each.
    let st = new ShardingTest({
        shards: {rs0: {nodes: 3}, rs1: {nodes: 3}},
        // By default, our test infrastructure sets the election timeout to a very high value (24
        // hours). For this test, we need a shorter election timeout because it relies on nodes
        // running an election when they do not detect an active primary. Therefore, we are setting
        // the electionTimeoutMillis to its default value.
        initiateWithDefaultElectionTimeout: true,
    });

    // Create a sharded collection with two chunks: [-inf, 50), [50, inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

    return st;
}

function writeRangeDeletionTask(collectionUuid, shardConn, pending, numOrphans, shardVersion) {
    let deletionTask = {
        _id: UUID(),
        nss: ns,
        collectionUuid: collectionUuid,
        donorShardId: "unused",
        numOrphanDocs: numOrphans,
        range: {min: {x: 50}, max: {x: MaxKey}},
        whenToClean: "now",
        preMigrationShardVersion: shardVersion,
    };

    if (pending) deletionTask.pending = true;

    let deletionsColl = shardConn.getCollection(rangeDeletionNs);

    // Write range to deletion collection
    assert.commandWorked(deletionsColl.insert(deletionTask));
}

(() => {
    jsTestLog("Test normal case where the pending field has been removed and the orphans are deleted");
    let st = setup();

    let testDB = st.s.getDB(dbName);
    let testColl = testDB.foo;

    // Move chunk [50, inf) to shard1.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName, _waitForDelete: true}),
    );

    let shard0Coll = st.shard0.getCollection(ns);

    // Write some orphaned documents directly to shard0.
    let orphanCount = 0;
    for (let i = 70; i < 90; i++) {
        assert.commandWorked(shard0Coll.insert({x: i}));
        ++orphanCount;
    }

    const expectedNumDocsTotal = 0;
    const expectedNumDocsShard0 = 0;
    const expectedNumDocsShard1 = 0;

    // Verify total count.
    assert.eq(testColl.find().itcount(), expectedNumDocsTotal);

    // Verify shard0 count includes orphans.
    assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0 + orphanCount);

    // Verify shard1 count.
    let shard1Coll = st.shard1.getCollection(ns);
    assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

    const collectionUuid = getUUIDFromConfigCollections(st.s, ns);
    const shardVersion = ShardVersioningUtil.getShardVersion(st.shard0, ns, true /* waitForRefresh */);
    writeRangeDeletionTask(collectionUuid, st.shard0, false, orphanCount, shardVersion);

    // Step down current primary.
    let originalShard0Primary = st.rs0.getPrimary();
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

(() => {
    jsTestLog("Test the case where pending: true and the orphans are NOT deleted");
    let st = setup();

    let testDB = st.s.getDB(dbName);
    let testColl = testDB.foo;

    // Move chunk [50, inf) to shard1.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName, _waitForDelete: true}),
    );

    let shard0Coll = st.shard0.getCollection(ns);

    // Write some orphaned documents directly to shard0.
    let orphanCount = 0;
    for (let i = 70; i < 90; i++) {
        assert.commandWorked(shard0Coll.insert({x: i}));
        ++orphanCount;
    }

    const collectionUuid = getUUIDFromConfigCollections(st.s, ns);
    const shardVersion = ShardVersioningUtil.getShardVersion(st.shard0, ns, true /* waitForRefresh */);
    writeRangeDeletionTask(collectionUuid, st.shard0, true, orphanCount, shardVersion);

    const expectedNumDocsTotal = 0;
    const expectedNumDocsShard0 = 0;
    const expectedNumDocsShard1 = 0;

    // Verify total count.
    assert.eq(testColl.find().itcount(), expectedNumDocsTotal);

    // Verify shard0 count includes orphans.
    assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0 + orphanCount);

    // Verify shard1 count.
    let shard1Coll = st.shard1.getCollection(ns);
    assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1);

    // Step down current primary.
    let originalShard0Primary = st.rs0.getPrimary();
    assert.commandWorked(originalShard0Primary.adminCommand({replSetStepDown: 60, force: 1}));

    // Connect to new primary for shard0.
    let shard0Primary = st.rs0.getPrimary();
    let shard0PrimaryColl = shard0Primary.getCollection(ns);

    // Verify that orphans are NOT deleted.
    assert.eq(shard0PrimaryColl.find().itcount(), expectedNumDocsShard0 + orphanCount);

    st.stop();
})();

/**
 * Verifies the list of shards targeted by multi writes based on queries and cluster
 * configurations.
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   requires_fcv_82,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const kDbName = "test_db";
const kCollName = "test_coll";
const kNs = kDbName + "." + kCollName;
const kNumDocs = 100;
const kDataChunkSplit = kNumDocs / 2;
const kOrphanDocs = 100;  // Number of orphaned documents to create on shard2

const st = new ShardingTest({shards: 3, other: {enableBalancer: false}});

// The test enforces inconsistent state across shards to validate the behavior of updates and
// deletes
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckDBHashes = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

/**
 * Sets up the data distribution:
 * shard0: [Min, 50[
 * shard1: [50, Max]
 * shard2: Orphaned documents [0,100]
 */
function setupDataDistribution() {
    const mongos = st.s;
    const db = mongos.getDB(kDbName);
    const coll = db.getCollection(kCollName);

    db.dropDatabase();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));

    assert.commandWorked(mongos.adminCommand({shardCollection: kNs, key: {x: 1}}));

    // Split at kDataChunkSplit and move the upper chunk to shard1
    assert.commandWorked(mongos.adminCommand({split: kNs, middle: {x: kDataChunkSplit}}));
    assert.commandWorked(mongos.adminCommand({
        moveChunk: kNs,
        find: {x: kDataChunkSplit + 1},
        to: st.shard1.shardName,
        _waitForDelete: true
    }));

    // Insert kNumDocs documents distributed across the two shards
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; i++) {
        bulk.insert({x: i, isOrphan: false});
    }
    assert.commandWorked(bulk.execute());

    // Create orphaned documents directly on shard2
    const shard2DB = st.shard2.getDB(kDbName);
    shard2DB.dropDatabase();
    const shard2Coll = shard2DB.getCollection(kCollName);

    bulk = shard2Coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kOrphanDocs; i++) {
        bulk.insert({x: i, isOrphan: true});
    }
    assert.commandWorked(bulk.execute());

    // Verify distribution
    assert.eq(kNumDocs, coll.countDocuments({}));  // Only shows official sharded docs, not orphans
    assert.eq(kOrphanDocs,
              shard2DB[kCollName].countDocuments({isOrphan: true}));  // Confirm orphans

    jsTest.log(`Data distribution setup complete: ${kNumDocs} documents across shards 0 and 1, ` +
               `${kOrphanDocs} orphaned documents on shard2`);

    // Ensure the router has latest routing info
    coll.find({}).itcount();
}

/**
 * Verifies the state of orphaned documents on shard2
 */
function checkUpdatedOrphans(expectedUpdatedCount) {
    const shard2DB = st.shard2.getDB(kDbName);
    const updatedOrphans = shard2DB[kCollName].countDocuments({isOrphan: true, updated: true});
    assert.eq(expectedUpdatedCount,
              updatedOrphans,
              `Expected ${expectedUpdatedCount} updated orphans, found ${updatedOrphans}`);
}

// First set up the data distribution
setupDataDistribution();

jsTest.log("updateMany targeting only one shard should not touch orphans");
{
    const mongos = st.s;
    const db = mongos.getDB(kDbName);
    const coll = db.getCollection(kCollName);

    // Update only documents on shard0
    assert.commandWorked(coll.updateMany({x: {$lt: kDataChunkSplit - 1}}, {$set: {updated: true}}));

    // Verify orphans were not updated
    checkUpdatedOrphans(0);

    // Restore data for subsequent tests
    setupDataDistribution();
}

jsTest.log("updateMany targeting all shards should update orphans");
{
    const mongos = st.s;
    const db = mongos.getDB(kDbName);
    const coll = db.getCollection(kCollName);

    // Update all documents, including orphans
    assert.commandWorked(coll.updateMany({}, {$set: {updated: true}}));

    // Verify orphans were updated
    checkUpdatedOrphans(kOrphanDocs);

    // Restore data for subsequent tests
    setupDataDistribution();
}

jsTest.log("deleteMany targeting one shard should not delete orphans");
{
    const mongos = st.s;
    const db = mongos.getDB(kDbName);
    const coll = db.getCollection(kCollName);

    // Count orphans before delete
    const shard2DB = st.shard2.getDB(kDbName);
    const orphanCountBefore = shard2DB[kCollName].countDocuments({isOrphan: true});

    // Delete documents only on shard0
    assert.commandWorked(coll.deleteMany({x: {$lt: 10}}));  // Delete a few docs from shard0

    // Verify orphans were not deleted
    const orphanCountAfter = shard2DB[kCollName].countDocuments({isOrphan: true});
    assert.eq(orphanCountBefore,
              orphanCountAfter,
              "Orphan count changed unexpectedly during single-shard delete");

    // Restore data for subsequent tests
    setupDataDistribution();
}

jsTest.log("deleteMany targeting multiple shards should delete orphans");
{
    const mongos = st.s;
    const db = mongos.getDB(kDbName);
    const coll = db.getCollection(kCollName);

    // Count orphans before delete
    const shard2DB = st.shard2.getDB(kDbName);
    const orphanCountBefore = shard2DB[kCollName].countDocuments({isOrphan: true});
    assert.gt(orphanCountBefore, 0, "Expected orphans for test to be valid");

    // Delete all documents including orphans
    assert.commandWorked(coll.deleteMany({}));

    // Verify orphans were deleted
    const orphanCountAfter = shard2DB[kCollName].countDocuments({isOrphan: true});
    assert.eq(
        0, orphanCountAfter, "Expected all orphans to be deleted, but found " + orphanCountAfter);

    // Restore data for subsequent tests
    setupDataDistribution();
}

jsTest.log("Enabling onlyTargetDataOwningShardsForMultiWrites feature");
assert.commandWorked(st.s.adminCommand(
    {setClusterParameter: {onlyTargetDataOwningShardsForMultiWrites: {enabled: true}}}));

assert.commandWorked(
    st.s.adminCommand({getClusterParameter: "onlyTargetDataOwningShardsForMultiWrites"}));

jsTest.log("With onlyTargetDataOwningShardsForMultiWrites, updateMany should not touch orphans");
{
    const mongos = st.s;
    const db = mongos.getDB(kDbName);
    const coll = db.getCollection(kCollName);

    // Update all documents
    assert.commandWorked(coll.updateMany({}, {$set: {updated: true}}));

    // Verify orphans were not updated
    checkUpdatedOrphans(0);

    // Restore data for subsequent tests
    setupDataDistribution();
}

jsTest.log("With onlyTargetDataOwningShardsForMultiWrites, deleteMany should not delete orphans");
{
    const mongos = st.s;
    const db = mongos.getDB(kDbName);
    const coll = db.getCollection(kCollName);

    // Count orphans before delete
    const shard2DB = st.shard2.getDB(kDbName);
    const orphanCountBefore = shard2DB[kCollName].countDocuments({isOrphan: true});
    assert.gt(orphanCountBefore, 0, "Expected orphans for test to be valid");

    // Delete all visible documents
    assert.commandWorked(coll.deleteMany({}));

    // Verify orphans were not deleted
    const orphanCountAfter = shard2DB[kCollName].countDocuments({isOrphan: true});
    assert.eq(orphanCountBefore,
              orphanCountAfter,
              "Expected orphans to remain with onlyTargetDataOwningShardsForMultiWrites enabled");

    jsTest.log(
        "Test passed: orphans were not deleted with onlyTargetDataOwningShardsForMultiWrites enabled");
}

st.stop();

/**
 * Tests the multi writes when a migrations occur in between a yield and resume.
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   requires_fcv_82,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Define the collection name and database
const kDbName = "test_db";
const kCollName = "test_coll";
const kNs = kDbName + "." + kCollName;
const kNumDocs = 100;
const kDataChunkSplit = kNumDocs / 2;
const kEmptyChunk = -10;
const kDataChunk1 = kDataChunkSplit - 10;
const kDataChunk2 = kDataChunkSplit + 10;

// Configure 'internalQueryExecYieldIterations' on both shards such that operations will yield on
// each 10th PlanExecuter iteration.
const st = new ShardingTest({
    shards: 3,
    rs: {setParameter: {internalQueryExecYieldIterations: 11}},
    other: {enableBalancer: false}
});

/*
 * Sets up the data distribution for testing migration conflicts
 * shard0: [Min, -1], [0,50[
 * shard1: [50,100]
 * shard2: []
 * on shard0 the first chunk is always empty. That chunk will be moved and used to bump the shard
 * version.
 */
function setupDataDistribution() {
    const mongos = st.s;
    const db = mongos.getDB(kDbName);
    const coll = db.getCollection(kCollName);

    db.dropDatabase();

    // Set shard0 as primary
    assert.commandWorked(
        mongos.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));

    assert.commandWorked(mongos.adminCommand({shardCollection: kNs, key: {x: 1}}));

    // Insert kNumDocs documents, evenly distributed across the two shards
    assert.commandWorked(mongos.adminCommand({split: kNs, middle: {x: kDataChunkSplit}}));
    assert.commandWorked(mongos.adminCommand({split: kNs, middle: {x: -1}}));

    assert.commandWorked(mongos.adminCommand(
        {moveChunk: kNs, find: {x: kDataChunk2}, to: st.shard1.shardName, _waitForDelete: true}));

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; i++) {
        bulk.insert({x: i, value: "test value " + i});
    }
    assert.commandWorked(bulk.execute());

    // Verify distribution
    assert.eq(kNumDocs, coll.countDocuments({}));
    jsTest.log(`Data distribution setup complete: ${kNumDocs} documents, ${
        kNumDocs / 2} on shard0, ${kNumDocs / 2} on shard1`);

    // Ensure the router has latest routing info.
    coll.find({});
}

// Configure fail point to hang on yield. While hanging, we can perform a migration. On resume,
// the shard will find a new version and throw a StaleConfig. Specifically for multi write, we
// want that StaleConfig to result in a QueryPlanKilled or silently succeed based on the cluster
// configuration.
function runTest(st, testCaseFun) {
    // Cleanup - reset data distribution
    setupDataDistribution();

    const mongos = st.s;
    const db = mongos.getDB(kDbName);
    const coll = db.getCollection(kCollName);

    const fpShard0UpdateHang = configureFailPoint(
        st.rs0.getPrimary(), 'setYieldAllLocksHang', {namespace: coll.getFullName()});

    const writeShell = startParallelShell(
        funWithArgs(testCaseFun, kDbName, kCollName, kDataChunk1, kDataChunk2), mongos.port);

    // Wait for the fail points to be hit
    jsTest.log("Waiting for operation to yield");
    fpShard0UpdateHang.wait();
    jsTest.log("Operation yielded, running migration...");

    // Migrate a chunk from shard0 to shard2
    jsTest.log("Starting migration.");
    assert.commandWorked(
        mongos.adminCommand({moveChunk: kNs, find: {x: kEmptyChunk}, to: st.shard2.shardName}));
    jsTest.log("Completed migration.");

    jsTest.log("Migration complete, resuming operation");
    fpShard0UpdateHang.off();
    jsTest.log("Waiting for operation to complete");

    writeShell();
    jsTest.log("Operation completed");
}

jsTest.log("updateMany multi:true with concurrent migration should fail if targets 1 shard");
{
    // The multi write targets 1 shard. We expect the operation to use a valid ShardVersion and
    // throw StaleConfig. However, since we can't safely retry updateMany, the operation will fail
    // with QueryPlanKilled instead.
    let testCase = function(dbName, collName, chunk1, chunk2) {
        assert.throwsWithCode(() => {
            db.getSiblingDB(dbName)[collName].updateMany({x: {$lt: chunk1}},  // Target only shard0
                                                         {$set: {updated: true}});
        }, ErrorCodes.QueryPlanKilled);
    };
    runTest(st, testCase);
}

jsTest.log("updateMany multi:true with concurrent migration should succeed if targets N shards");
{
    // The multi write targets multiple shards. We expect the operation to use ShardVersion::IGNORED
    // and always succeed indipendently that a placement change happened during the execution.
    let testCase = function(dbName, collName, chunk1, chunk2) {
        assert.commandWorked(
            db.getSiblingDB(dbName)[collName].updateMany({x: {$gt: -1}},  // Targets all documents
                                                         {$set: {updated: true}}));
    };
    runTest(st, testCase);
}

jsTest.log("deleteMany multi:true with concurrent migration succeed if targets 1 shard");
{
    // The deleteMany targets 1 shard. Like updateMany, we expect the operation to use a valid
    // ShardVersion and throw StaleConfig, resulting in QueryPlanKilled error.
    let testCase = function(dbName, collName, chunk1, chunk2) {
        assert.commandWorked(db.getSiblingDB(dbName)[collName].deleteMany(
            {x: {$lt: chunk1}}));  // Targets all documents
    };
    runTest(st, testCase);
}

jsTest.log("deleteMany multi:true with concurrent migration should succeed if targets N shards");
{
    // The deleteMany targets multiple shards. We expect the operation to use ShardVersion::IGNORED
    // and succeed despite the placement change during execution.
    let testCase = function(dbName, collName, chunk1, chunk2) {
        assert.commandWorked(
            db.getSiblingDB(dbName)[collName].deleteMany({x: {$gt: -1}}));  // Targets all documents
    };
    runTest(st, testCase);
}

// Enable the cluster parameter that changes how distributed multi-writes targets multiple shards
jsTest.log("Enabling onlyTargetDataOwningShardsForMultiWrites");
assert.commandWorked(st.s.adminCommand(
    {setClusterParameter: {onlyTargetDataOwningShardsForMultiWrites: {enabled: true}}}));
assert.commandWorked(
    st.s.adminCommand({getClusterParameter: "onlyTargetDataOwningShardsForMultiWrites"}));

jsTest.log(
    "updateMany with concurrent migration should fail with QueryPlanKilled when onlyTargetDataOwningShardsForMultiWrites enabled");
{
    let testCase = function(dbName, collName, chunk1, chunk2) {
        assert.throwsWithCode(() => {
            assert.commandWorked(db.getSiblingDB(dbName)[collName].updateMany(
                {x: {$gt: -1}},  // Targets all documents
                {$set: {updated: true}}));
        }, ErrorCodes.QueryPlanKilled);
    };
    runTest(st, testCase);
}

jsTest.log(
    "deleteMany with concurrent migration should still return ok when onlyTargetDataOwningShardsForMultiWrites enabled");
{
    let testCase = function(dbName, collName, chunk1, chunk2) {
        assert.commandWorked(db.getSiblingDB(dbName)[collName].deleteMany({}));
    };
    runTest(st, testCase);
}

st.stop();

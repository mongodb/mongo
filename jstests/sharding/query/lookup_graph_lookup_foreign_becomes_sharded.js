/**
 * Tests that $lookup and $graphLookup correctly throw an exception if the foreign collection is
 * sharded, or if it becomes sharded mid-iteration.
 *
 *@tags: [
 *   requires_persistence
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");         // For assertErrorCode.
load("jstests/libs/profiler.js");                    // For profilerHasSingleMatchingEntryOrThrow.
load("jstests/multiVersion/libs/multi_cluster.js");  // For ShardingTest.waitUntilStable.

// Currently, even if the 'featureFlagShardedLookup' flag is enabled, $graphLookup into a
// sharded collection is not supported (testing this functionality should assert that the
// command fails). This will be fixed by SERVER-58073.

const st = new ShardingTest({shards: 1, mongos: 2, rs: {nodes: 1}});
const shard0 = st.rs0;

const freshMongos = st.s0.getDB(jsTestName());
const staleMongos = st.s1.getDB(jsTestName());

const sourceCollection = freshMongos.source;
const foreignCollection = freshMongos.foreign;

// Enable sharding on the test database, but do not shard any collections yet.
assert.commandWorked(freshMongos.adminCommand({enableSharding: freshMongos.getName()}));

// Batch size used for all requests issued during this test.
const batchSize = 2;

// Helper function to recreate and repopulate the source and foreign collections.
function setupBothMongos() {
    // Make sure the number of documents inserted is greater than the batchSize, to ensure the
    // results cannot all fit in one batch.
    const numMatches = batchSize + 4;

    // Drop the source and foreign collections.
    sourceCollection.drop();
    foreignCollection.drop();

    // Insert some test documents into both collections. Alternate the insertions between the fresh
    // and stale mongos so that both have a view of the foreign collection as unsharded.
    for (let i = 0; i < numMatches; ++i) {
        assert.commandWorked(sourceCollection.insert({_id: i, local: i}));
        for (let j = 0; j < numMatches; ++j) {
            const mongosForInsert = (j % 2 ? freshMongos : staleMongos);
            assert.commandWorked(mongosForInsert[foreignCollection.getName()].insert(
                {_id: numMatches * i + j, foreign: i}));
        }
    }
}

// Set up the test cases. We use the 'localField / foreignField' form of $lookup because it creates
// a pipeline whose $match stage is correlated with the foreign collection; this means that we
// cannot cache any results, and so each getMore must consult the foreign collection directly.
// Similarly, we make sure the documents in the $graphLookup match 1:1. Otherwise, the results for
// getMore will be taken from the cache and the operation will remain unaware that the foreign
// collection has became sharded mid-iteration.
const testCases = [{
        aggCmd: {
        aggregate: sourceCollection.getName(),
        pipeline: [{
            $lookup: {
                from: foreignCollection.getName(),
                localField: 'local',
                foreignField: 'foreign',
                as: 'results',
            }
        }, ],
        cursor: {
            batchSize: batchSize,
        },
    },
    errCode: 51069
}, {
    aggCmd: {
        aggregate: sourceCollection.getName(),
        pipeline: [{
            $graphLookup: {
                from: foreignCollection.getName(),
                startWith: "$_id",
                connectFromField: "_id",
                connectToField: "_id",
                as: "res"
            }
        }],
        cursor: {
            batchSize: batchSize,
        },
    },
    errCode: 31428
}];

// Drop and recreate both collections, so that both mongos know the foreign collection is unsharded.
setupBothMongos();

// Verify that the 'aggregate' command works and produces (batchSize) results, indicating that the
// $lookup or $graphLookup succeeded on the unsharded foreign collection.
for (let testCase of testCases) {
    const aggCmdRes = assert.commandWorked(freshMongos.runCommand(testCase.aggCmd));
    assert.eq(aggCmdRes.cursor.firstBatch.length, batchSize);
    testCase.aggCmdRes = aggCmdRes;
}

// Use freshMongos to shard the foreignCollection, so staleMongos still thinks it is unsharded.
assert.commandWorked(
    freshMongos.adminCommand({shardCollection: foreignCollection.getFullName(), key: {_id: 1}}));

const getShardedLookupParam = st.s.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;

let res = st.getPrimaryShard(jsTestName()).getDB("admin").adminCommand({
    getParameter: 1,
    featureFlagSBELookupPushdown: 1,
    internalQueryForceClassicEngine: 1
});
let isSBELookupEnabled = res.ok && res.hasOwnProperty("featureFlagSBELookupPushdown") &&
    res.hasOwnProperty("internalQueryForceClassicEngine") &&
    res.featureFlagSBELookupPushdown.value && !res.internalQueryForceClassicEngine;

// Now run a getMore for each of the test cases. The collection has become sharded mid-iteration, so
// we should observe the error code associated with the test case.
// TODO SERVER-60018: When the feature flag is removed, assert that the command works and has
// expected results.
if (!isShardedLookupEnabled) {
    for (let testCase of testCases) {
        // In the classic lookup, when we compile an internal $match stage for $lookup local
        // document match against the foreign collection, we try to get a lock on the foreign
        // collection as the MAIN collection and in the SBE lookup, we try to get a lock on both the
        // local (MAIN) and foreign (secondary) collection. The thing is we go through different
        // checks for the main collection and secondary namespaces. For the main collection, we
        // check the shard version explicitly. For the secondary namespaces, we check sharding
        // changes based on the snapshot. So, we don't get the error and the SBE lookup succeeds at
        // getMore request. This is a similar situation as SERVER-64128 though it's not exactly
        // same. Until SERVER-64128 is resolved, the expected behavior is not exactly clear and we
        // need to revisit this scenario after SERVER-64128 is resolved. Until then, just checks
        // whether this scenario succeeds when the SBE lookup feature is enabled.
        if (isSBELookupEnabled && testCase.aggCmd.pipeline[0].hasOwnProperty("$lookup")) {
            assert.commandWorked(
                freshMongos.runCommand(
                    {getMore: testCase.aggCmdRes.cursor.id, collection: testCase.aggCmd.aggregate}),
                `Expected getMore to succeed. Original command: ${tojson(testCase.aggCmd)}`);
            continue;
        }

        assert.commandFailedWithCode(
            freshMongos.runCommand(
                {getMore: testCase.aggCmdRes.cursor.id, collection: testCase.aggCmd.aggregate}),
            testCase.errCode,
            `Expected getMore to fail. Original command: ${tojson(testCase.aggCmd)}`);
    }
}

// Run both test cases again. The fresh mongos knows that the foreign collection is sharded now, so
// both tests will fail on the mongos with error code 28769 without ever reaching the shard if the
// 'featureFlagShardedLookup' flag is disabled.
// TODO SERVER-60018: When the feature flag is removed, assert that the command works and has
// expected results.
if (!isShardedLookupEnabled) {
    for (let testCase of testCases) {
        assert.commandFailedWithCode(
            freshMongos.runCommand(testCase.aggCmd),
            28769,
            `Expected command to fail on mongos: ${tojson(testCase.aggCmd)}`);
    }
}

// Run the test cases through the stale mongos. It should still believe that the foreign collection
// is unsharded, and so it will send the aggregate command to the shard, where it will hit an error
// as soon as it attempts to access the foreign collection.
// TODO SERVER-60018: When the feature flag is removed, assert that the command works and has
// expected results.
if (!isShardedLookupEnabled) {
    for (let testCase of testCases) {
        assert.commandFailedWithCode(
            staleMongos.runCommand(testCase.aggCmd),
            testCase.errCode,
            `Expected command to fail on shard: ${tojson(testCase.aggCmd)}`);
    }
}

// Reset both collections to unsharded and make sure both mongos know.
setupBothMongos();

// Ensure the replication recover process isn't needed after restarting the shard. The recovery
// process can cause shards to reload their metadata and view the collections as unsharded rather
// than unknown.
shard0.awaitLastOpCommitted();
assert.commandWorked(shard0.getPrimary().adminCommand({fsync: 1}));

// Now restart the shard's Primary. This will have no metadata loaded on startup, and so we expect
// that the first attempt to run $lookup or $graphLookup should produce a stale shard exception for
// both the source and foreign collections.
shard0.restart(shard0.getPrimary());

// Refreshes the SBE lookup feature availability just in case that a different version of mongod
// is running after restart.
res = shard0.getPrimary().getDB("admin").adminCommand(
    {getParameter: 1, featureFlagSBELookupPushdown: 1});
isSBELookupEnabled = res.ok && res.hasOwnProperty("featureFlagSBELookupPushdown") &&
    res.featureFlagSBELookupPushdown.value;

// Enable profiling on shard0 to capture stale shard version exceptions.
const primaryDB = shard0.getPrimary().getDB(jsTestName());
assert.commandWorked(primaryDB.setProfilingLevel(2));

// Wait until both mongos have refreshed their view of the new Primary.
st.waitUntilStable();

const prevStaleConfigErrorCount = assert.commandWorked(primaryDB.runCommand({serverStatus: 1}))
                                      .shardingStatistics.countStaleConfigErrors;

// Verify that the 'aggregate' command works and produces (batchSize) results, indicating that the
// $lookup or $graphLookup succeeded on the unsharded foreign collection.
for (let testCase of testCases) {
    const aggCmdRes = assert.commandWorked(freshMongos.runCommand(testCase.aggCmd));
    assert.eq(aggCmdRes.cursor.firstBatch.length, batchSize);
}

const newStaleConfigErrorCount = assert.commandWorked(primaryDB.runCommand({serverStatus: 1}))
                                     .shardingStatistics.countStaleConfigErrors;

// ... and a single StaleConfig error for the foreign namespace. Note that the 'ns' field of the
// profiler entry is the source collection in both cases, because the $lookup's parent aggregation
// produces the profiler entry, and it is always running on the source collection.
// TODO SERVER-60018: When the feature flag is removed, remove the check and ensure the results are
// expected.
if (!isShardedLookupEnabled) {
    // Both in the classic lookup and the SBE lookup, StaleConfig error happens. In the classic
    // lookup, profiling can properly report the StaleConfig of the foreign collection.
    //
    // On the other hand, in the SBE lookup, profiling fails to report the error because the
    // profiling level is not set up properly according to the configured profiling level in case of
    // lock acquisition exception. So, we check statusStatus' StaleConfig error count instead.
    assert.gt(newStaleConfigErrorCount,
              prevStaleConfigErrorCount,
              "StaleConfig errors must have happened");
    if (!isSBELookupEnabled) {
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: primaryDB,
            filter: {
                ns: sourceCollection.getFullName(),
                errCode: ErrorCodes.StaleConfig,
                errMsg: {$regex: `${foreignCollection.getFullName()} is not currently available`}
            }
        });
    }
}

st.stop();
}());

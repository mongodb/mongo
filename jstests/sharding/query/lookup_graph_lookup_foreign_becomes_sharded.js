/**
 * Tests that $lookup and $graphLookup correctly succeed if the foreign collection is sharded, or if
 * it becomes sharded mid-iteration.
 *
 *@tags: [
 *   requires_persistence
 * ]
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_cluster.js");  // For ShardingTest.waitUntilStable.

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

// Now run a getMore for each of the test cases. The foreign collection has become sharded mid-
// iteration, but we can recover from this and continue execution.
for (let testCase of testCases) {
    // In the classic lookup, when we compile an internal $match stage for $lookup local
    // document match against the foreign collection, we try to get a lock on the foreign
    // collection as the MAIN collection and in the SBE lookup, we try to get a lock on both the
    // local (MAIN) and foreign (secondary) collection. The thing is we go through different
    // checks for the main collection and secondary namespaces. For the main collection, we
    // check the shard version explicitly. For the secondary namespaces, we check sharding
    // changes based on the snapshot. So, SBE lookup succeeds at getMore request.
    //
    // This is a similar situation as SERVER-64128 though it's not exactly same. Until SERVER-64128
    // is resolved, the expected behavior is not exactly clear and we need to revisit this scenario
    // after SERVER-64128 is resolved. Until then, just checks whether this scenario succeeds.
    assert.commandWorked(
        freshMongos.runCommand(
            {getMore: testCase.aggCmdRes.cursor.id, collection: testCase.aggCmd.aggregate}),
        `Expected getMore to succeed. Original command: ${tojson(testCase.aggCmd)}`);
}

// Run both test cases again. The fresh mongos knows that the foreign collection is sharded now,
// and the query should still work.
for (let testCase of testCases) {
    const aggCmdRes =
        assert.commandWorked(freshMongos.runCommand(testCase.aggCmd),
                             `Expected command to succeed: ${tojson(testCase.aggCmd)}`);
    assert.eq(aggCmdRes.cursor.firstBatch.length, batchSize);
}

// Run the test cases through the stale mongos. It should still believe that the foreign collection
// is unsharded, but it should recover from this, and the query should still work.
for (let testCase of testCases) {
    const aggCmdRes =
        assert.commandWorked(staleMongos.runCommand(testCase.aggCmd),
                             `Expected command to succeed: ${tojson(testCase.aggCmd)}`);
    assert.eq(aggCmdRes.cursor.firstBatch.length, batchSize);
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

// ... and a single StaleConfig exception for the foreign namespace. These StalecConfig errors are
// not always reported in the profiler, but they are reflected in the serverStatus StaleConfig error
// count.
assert.gt(
    newStaleConfigErrorCount, prevStaleConfigErrorCount, "StaleConfig errors must have happened");
st.stop();
}());

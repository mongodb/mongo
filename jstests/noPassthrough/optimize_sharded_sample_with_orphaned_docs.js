/**
 * Confirms that the decision to run the optimized $sample stage takes the ratio of orphans to legit
 * documents into account. In particular, a shard which possesses *only* orphan documents does not
 * induce the infinite-loop behaviour detailed in SERVER-36871.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

// Deliberately inserts orphans outside of migration.
TestData.skipCheckOrphans = true;

load('jstests/libs/analyze_plan.js');  // For aggPlanHasStage().

// Set up a 2-shard cluster.
const st = new ShardingTest({name: jsTestName(), shards: 2, rs: {nodes: 1}});

// Obtain a connection to the mongoS and one direct connection to each shard.
const shard0 = st.rs0.getPrimary();
const shard1 = st.rs1.getPrimary();
const mongos = st.s;

const configDB = mongos.getDB("config");

const mongosDB = mongos.getDB(jsTestName());
const mongosColl = mongosDB.test;

const shard0DB = shard0.getDB(jsTestName());
const shard0Coll = shard0DB.test;

const shard1DB = shard1.getDB(jsTestName());
const shard1Coll = shard1DB.test;

const shard1AdminDB = shard1.getDB("admin");

const shardNames = [st.rs0.name, st.rs1.name];

// Helper function that runs a $sample aggregation, confirms that the results are correct, and
// verifies that the expected optimized or unoptimized $sample stage ran on each shard.
function runSampleAndConfirmResults({sampleSize, comment, expectedPlanSummaries}) {
    // Run the aggregation via mongoS with the given 'comment' parameter.
    assert.eq(mongosColl.aggregate([{$sample: {size: sampleSize}}], {comment: comment}).itcount(),
              sampleSize);

    // Obtain the explain output for the aggregation.
    const explainOut =
        assert.commandWorked(mongosColl.explain().aggregate([{$sample: {size: sampleSize}}]));

    // Verify that the expected $sample stage, optimized or unoptimized, ran on each shard.
    for (let idx in expectedPlanSummaries) {
        const shardExplain = explainOut.shards[shardNames[idx]];
        for (let planSummary of expectedPlanSummaries[idx]) {
            assert(aggPlanHasStage(shardExplain, planSummary));
        }
    }
}

// Enable sharding on the the test database and ensure that the primary is shard0.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), shard0.name);

// Shard the collection on {_id: 1}, split at {_id: 0} and move the empty upper chunk to shard1.
st.shardColl(mongosColl.getName(), {_id: 1}, {_id: 0}, {_id: 0}, mongosDB.getName());

// Write some documents to the lower chunk on shard0.
for (let i = (-200); i < 0; ++i) {
    assert.commandWorked(mongosColl.insert({_id: i}));
}

// Set a failpoint to hang after cloning documents to shard1 but before committing.
shard0DB.adminCommand({configureFailPoint: "moveChunkHangAtStep4", mode: "alwaysOn"});
shard1DB.adminCommand({configureFailPoint: "moveChunkHangAtStep4", mode: "alwaysOn"});

// Spawn a parallel shell to move the lower chunk from shard0 to shard1.
const awaitMoveChunkShell = startParallelShell(`
        assert.commandWorked(db.adminCommand({
            moveChunk: "${mongosColl.getFullName()}",
            find: {_id: -1},
            to: "${shard1.name}",
            waitForDelete: true
        }));
    `,
                                                   mongosDB.getMongo().port);

// Wait until we see that all documents have been cloned to shard1.
assert.soon(() => {
    return shard0Coll.find().itcount() === shard1Coll.find().itcount();
});

// Confirm that shard0 still owns the chunk, according to the config DB metadata.
assert.eq(configDB.chunks.count({max: {_id: 0}, shard: `${jsTestName()}-rs0`}), 1);

// Run a $sample aggregation without committing the chunk migration. We expect to see that the
// optimized $sample stage was used on shard0, which own the documents. Despite the fact that
// there are 200 documents on shard1 and we should naively have used the random-cursor
// optimization, confirm that we instead detected that the documents were orphans and used the
// non-optimized $sample stage.
runSampleAndConfirmResults({
    sampleSize: 1,
    comment: "sample_with_only_orphans_on_shard1",
    expectedPlanSummaries: [["QUEUED_DATA", "MULTI_ITERATOR"], ["COLLSCAN"]]
});

// Confirm that shard0 still owns the chunk.
assert.eq(configDB.chunks.count({max: {_id: 0}, shard: `${jsTestName()}-rs0`}), 1);

// Release the failpoints and wait for the parallel moveChunk shell to complete.
shard0DB.adminCommand({configureFailPoint: "moveChunkHangAtStep4", mode: "off"});
shard1DB.adminCommand({configureFailPoint: "moveChunkHangAtStep4", mode: "off"});
awaitMoveChunkShell();

// Confirm that shard1 now owns the chunk.
assert.eq(configDB.chunks.count({max: {_id: 0}, shard: `${jsTestName()}-rs1`}), 1);

// Move the lower chunk back to shard0.
assert.commandWorked(mongosDB.adminCommand(
    {moveChunk: mongosColl.getFullName(), find: {_id: -1}, to: shard0.name, waitForDelete: true}));

// Write 1 legitimate document and 100 orphans directly to shard1, which owns the upper chunk.
assert.eq(configDB.chunks.count({min: {_id: 0}, shard: `${jsTestName()}-rs1`}), 1);
for (let i = -100; i < 1; ++i) {
    assert.commandWorked(shard1Coll.insert({_id: i}));
}

// Confirm that there are 101 documents on shard1 and mongoS can see the 1 non-orphan.
assert.eq(mongosColl.find({_id: {$gte: 0}}).itcount(), 1);
assert.eq(shard1Coll.count(), 101);

// Re-run the $sample aggregation. On shard1 we should again use the non-optimized stage, since
// despite the fact that there are 101 documents present, only 1 is owned by the shard.
runSampleAndConfirmResults({
    sampleSize: 1,
    comment: "sample_with_1_doc_100_orphans_on_shard1",
    expectedPlanSummaries: [["QUEUED_DATA", "MULTI_ITERATOR"], ["COLLSCAN"]]
});

// Write 199 additional documents to the upper chunk which still resides on shard1.
assert.eq(configDB.chunks.count({min: {_id: 0}, shard: `${jsTestName()}-rs1`}), 1);
for (let i = 1; i < 200; ++i) {
    assert.commandWorked(mongosColl.insert({_id: i}));
}

// Re-run the $sample aggregation and confirm that the optimized stage now runs on both shards.
runSampleAndConfirmResults({
    sampleSize: 1,
    comment: "sample_with_200_docs_100_orphans_on_shard1",
    expectedPlanSummaries: [["QUEUED_DATA", "MULTI_ITERATOR"], ["QUEUED_DATA", "MULTI_ITERATOR"]]
});

st.stop();
})();

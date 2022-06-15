/**
 * Confirms that a sharded $sample which employs the DSSampleFromRandomCursor optimization is
 * capable of yielding.
 *
 * @tags: [assumes_read_concern_unchanged, do_not_wrap_aggregations_in_facets,
 * requires_sharding]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

// Set up a 2-shard cluster. Configure 'internalQueryExecYieldIterations' on both shards such
// that operations will yield on each PlanExecuter iteration.
const st = new ShardingTest({
    name: jsTestName(),
    shards: 2,
    rs: {nodes: 1, setParameter: {internalQueryExecYieldIterations: 1}}
});

const mongosDB = st.s.getDB(jsTestName());
const mongosColl = mongosDB.test;

// Shard the test collection, split it at {_id: 0}, and move the upper chunk to shard1.
st.shardColl(mongosColl, {_id: 1}, {_id: 0}, {_id: 0});

// Insert enough documents on each shard to induce the $sample random-cursor optimization.
for (let i = (-150); i < 150; ++i) {
    assert.commandWorked(mongosColl.insert({_id: i}));
}

// Run the initial aggregate for the $sample stage.
const cmdRes = assert.commandWorked(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [{$sample: {size: 3}}],
    comment: "$sample random",
    cursor: {batchSize: 0}
}));
assert.eq(cmdRes.cursor.firstBatch.length, 0);

// Force each shard to hang on yield to allow for currentOp capture.
FixtureHelpers.runCommandOnEachPrimary({
    db: mongosDB.getSiblingDB("admin"),
    cmdObj: {
        configureFailPoint: "setYieldAllLocksHang",
        mode: "alwaysOn",
        data: {namespace: mongosColl.getFullName()}
    }
});

// Run $currentOp to confirm that the $sample getMore yields on both shards.
const awaitShell = startParallelShell(() => {
    load("jstests/libs/fixture_helpers.js");
    assert.soon(() => db.getSiblingDB("admin")
                          .aggregate([
                              {$currentOp: {}},
                              {
                                  $match: {
                                      "cursor.originatingCommand.comment": "$sample random",
                                      planSummary: "QUEUED_DATA, MULTI_ITERATOR",
                                      numYields: {$gt: 0}
                                  }
                              }
                          ])
                          .itcount() === 2);
    // Release the failpoint and allow the getMores to complete.
    FixtureHelpers.runCommandOnEachPrimary({
        db: db.getSiblingDB("admin"),
        cmdObj: {configureFailPoint: "setYieldAllLocksHang", mode: "off"}
    });
}, mongosDB.getMongo().port);

// Retrieve the results for the $sample aggregation.
const sampleCursor = new DBCommandCursor(mongosDB, cmdRes);
assert.eq(sampleCursor.toArray().length, 3);

// Confirm that the parallel shell completes successfully, and tear down the cluster.
awaitShell();
st.stop();
})();

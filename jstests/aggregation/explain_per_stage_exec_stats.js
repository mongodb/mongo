/**
 * Tests that aggregation stages report the number of documents returned (nReturned) and
 * execution time (executionTimeMillisEstimate) when explain is run with verbosities
 * "executionStats" and "allPlansExecution".
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");     // For getAggPlanStages().
load("jstests/libs/fixture_helpers.js");  // For isReplSet().

const coll = db.explain_per_stage_exec_stats;
coll.drop();

let bulk = coll.initializeUnorderedBulkOp();
const nDocs = 1000;
for (let i = 0; i < nDocs; i++) {
    bulk.insert({_id: i, a: i, b: i % 50});
}
assert.commandWorked(bulk.execute());

const pipelineShardedStages = [
    {$match: {a: {$gt: 500}}},
    {$addFields: {arr: [1, 2]}},
    {$unwind: {path: "$arr"}},
    {$sort: {a: -1}},
    {$skip: 100},
    {$project: {arr: 0}},
    {$group: {_id: "$b", count: {$sum: 1}}},
    {$limit: 10}
];

// Test explain output where the shards part of the pipeline can be optimized away.
//
// By having a $skip after a $match, each shard will only execute $match which will
// yield "queryPlanner" and "executionStats" instead of a list of stages.
const pipelineNoShardedStages = [
    {$match: {a: {$gt: 500}}},
    {$skip: 100},
    {$addFields: {arr: [1, 2]}},
    {$unwind: {path: "$arr"}},
    {$sort: {a: -1}},
    {$project: {arr: 0}},
    {$group: {_id: "$b", count: {$sum: 1}}},
    {$limit: 10}
];

// Verify behavior of a nested pipeline.
const facet = [{$facet: {a: pipelineShardedStages, b: pipelineNoShardedStages}}];

// Verify behavior of $changeStream, which generates several internal stages.
const changeStream = [{$changeStream: {}}];

// Checks if a particular stage has expected statistics.
function assertStageExecutionStatsPresent(stage) {
    if (stage.hasOwnProperty("$sort")) {
        assert(stage.hasOwnProperty("totalDataSizeSortedBytesEstimate"), stage);
        assert(stage.hasOwnProperty("usedDisk"), stage);
    } else if (stage.hasOwnProperty("$group")) {
        assert(stage.hasOwnProperty("totalOutputDataSizeBytes"), stage);
        assert(stage.hasOwnProperty("usedDisk"), stage);
    }
}

function assertExecutionStats(stage, assertExecutionStatsCallback) {
    assert(stage.hasOwnProperty("nReturned"));
    assert(stage.hasOwnProperty("executionTimeMillisEstimate"));

    assert.neq(assertExecutionStatsCallback, null);
    assertExecutionStatsCallback(stage);
}

function assertStatsInOutput(explain, assertExecutionStatsCallback) {
    // Depending on how the pipeline is split, the explain output from each shard can contain either
    // of these.
    assert(explain.hasOwnProperty("stages") || explain.hasOwnProperty("queryPlanner"));
    if (explain.hasOwnProperty("stages")) {
        const stages = explain["stages"];
        for (const stage of stages) {
            assertExecutionStats(stage, assertExecutionStatsCallback);
        }
    } else {
        // If we don't have a list of stages, "executionStats" should still contain "nReturned"
        // and "executionTimeMillisEstimate". Also, "queryPlanner" should report that
        // optimizedPipeline is set to true.
        assert(explain.hasOwnProperty("executionStats"));
        const execStats = explain["executionStats"];
        assert(execStats.hasOwnProperty("nReturned"));
        assert(execStats.hasOwnProperty("executionTimeMillis"));

        const queryPlanner = explain["queryPlanner"];
        assert(queryPlanner.hasOwnProperty("optimizedPipeline"));
        assert.eq(queryPlanner["optimizedPipeline"], true);
    }
}

function checkResults(result, assertExecutionStatsCallback) {
    // Loop over shards in sharded case.
    // Note that we do not expect execution statistics for the 'splitPipeline' or 'mergingPart'
    // of explain output in the sharded case, so we only check the 'shards' part of explain.
    if (result.hasOwnProperty("shards")) {
        const shards = result["shards"];
        for (let shardName in shards) {
            assertStatsInOutput(shards[shardName], assertExecutionStatsCallback);
        }
    } else {
        assertStatsInOutput(result, assertExecutionStatsCallback);
    }
}

for (let pipeline of [pipelineShardedStages, pipelineNoShardedStages, facet]) {
    checkResults(coll.explain("executionStats").aggregate(pipeline),
                 assertStageExecutionStatsPresent);
    checkResults(coll.explain("allPlansExecution").aggregate(pipeline),
                 assertStageExecutionStatsPresent);
}

// Only test $changeStream if we are on a replica set or on a sharded cluster.
if (FixtureHelpers.isReplSet(db) || FixtureHelpers.isSharded(coll)) {
    checkResults(coll.explain("executionStats").aggregate(changeStream),
                 assertStageExecutionStatsPresent);
    checkResults(coll.explain("allPlansExecution").aggregate(changeStream),
                 assertStageExecutionStatsPresent);
}

// Returns the number of documents
function numberOfDocsReturnedByMatchStage(explain) {
    let sum = 0;
    const matchStages = getAggPlanStages(explain, "$match");
    assert.neq(null, matchStages, "Could not find $match in explain output: " + explain);
    for (const stage of matchStages) {
        assert(stage.hasOwnProperty("nReturned"));
        sum += stage["nReturned"];
    }
    return sum;
}

const matchPipeline = [{$_internalInhibitOptimization: {}}, {$match: {a: {$gte: 500}}}];
assert.eq(numberOfDocsReturnedByMatchStage(coll.explain("executionStats").aggregate(matchPipeline)),
          500);

// Checks $group totalOutputDataSizeBytes execution statistic.
(function testGroupStatTotalDataSizeBytes() {
    const pipeline = [{$group: {_id: null, count: {$sum: 1}}}];
    const result = coll.explain("executionStats").aggregate(pipeline);

    let assertOutputBytesSize = function(stage) {
        if (stage.hasOwnProperty("$group")) {
            assert(stage.hasOwnProperty("totalOutputDataSizeBytes"), stage);

            // A heurisitic size in bytes processed by $group to generate the output '{ "_id" :
            // null, "count" : 1000 }'. The size is the approximate value of internal document size
            // used by $group.
            const approximateOutputDocSizeBytes = 500;
            assert(stage.totalOutputDataSizeBytes <= approximateOutputDocSizeBytes);
        }
    };
    checkResults(result, assertOutputBytesSize);
})();
}());

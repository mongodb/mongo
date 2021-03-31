/**
 * Tests that $group stage reports memory footprint per accumulator when explain is run with
 * verbosities "executionStats" and "allPlansExecution".
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStages().

const conn = MongoRunner.runMongod();
const testDB = conn.getDB('test');
const coll = testDB.explain_group_stage_exec_stats;
coll.drop();
const bigStr = Array(1025).toString();  // 1KB of ','
const maxMemoryLimitForGroupStage = 1024 * 300;
const debugBuild = testDB.adminCommand('buildInfo').debug;
const nDocs = 1000;
const nGroups = 50;

let bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({_id: i, a: i, b: i % nGroups, bigStr: bigStr});
}
assert.commandWorked(bulk.execute());

const pipeline = [
    {$match: {a: {$gt: 0}}},
    {$group: {_id: "$b", count: {$sum: 1}, push: {$push: "$bigStr"}, set: {$addToSet: "$bigStr"}}},
];

const expectedAccumMemUsages = {
    count: nGroups * 60,
    push: nDocs * 1024,
    set: nGroups * 1024,
};

/**
 * Checks that the execution stats in the explain output for a $group stage are as expected.
 * - 'stages' is an array of the explain output of $group stages.
 * - 'expectedAccumMemUsages' is used to check the memory footprint stats for each accumulator.
 * - 'isExecExplain' indicates that the explain output is run with verbosity "executionStats" or
 * "allPlansExecution".
 * - 'shouldSpillToDisk' indicates data was spilled to disk when executing $group stage.
 */
function checkGroupStages(stages, expectedAccumMemUsages, isExecExplain, shouldSpillToDisk) {
    // Tracks the memory usage per accumulator in total as 'stages' passed in could be the explain
    // output across a cluster.
    let totalAccumMemoryUsageBytes = 0;

    for (let stage of stages) {
        assert(stage.hasOwnProperty("$group"), stage);

        if (isExecExplain) {
            assert(stage.hasOwnProperty("maxAccumulatorMemoryUsageBytes"), stage);
            const maxAccmMemUsages = stage["maxAccumulatorMemoryUsageBytes"];
            for (let field of Object.keys(maxAccmMemUsages)) {
                totalAccumMemoryUsageBytes += maxAccmMemUsages[field];

                // Ensures that the expected accumulators are all included and the corresponding
                // memory usage is in a reasonable range. Note that in debug mode, data will be
                // spilled to disk every time we add a new value to a pre-existing group.
                if (!debugBuild && expectedAccumMemUsages.hasOwnProperty(field)) {
                    assert.gt(maxAccmMemUsages[field], expectedAccumMemUsages[field]);
                    assert.lt(maxAccmMemUsages[field], 5 * expectedAccumMemUsages[field]);
                }
            }
        } else {
            assert(!stage.hasOwnProperty("maxAccumulatorMemoryUsageBytes"), stage);
        }
    }

    // Add some wiggle room to the total memory used compared to the limit parameter since the check
    // for spilling to disk happens after each document is processed.
    if (shouldSpillToDisk)
        assert.gt(
            maxMemoryLimitForGroupStage + 3 * 1024, totalAccumMemoryUsageBytes, tojson(stages));
}

let groupStages = getAggPlanStages(coll.explain("executionStats").aggregate(pipeline), "$group");
checkGroupStages(groupStages, expectedAccumMemUsages, true, false);

groupStages = getAggPlanStages(coll.explain("allPlansExecution").aggregate(pipeline), "$group");
checkGroupStages(groupStages, expectedAccumMemUsages, true, false);

groupStages = getAggPlanStages(coll.explain("queryPlanner").aggregate(pipeline), "$group");
checkGroupStages(groupStages, {}, false, false);

// Set MaxMemory low to force spill to disk.
assert.commandWorked(testDB.adminCommand(
    {setParameter: 1, ["internalDocumentSourceGroupMaxMemoryBytes"]: maxMemoryLimitForGroupStage}));

groupStages = getAggPlanStages(
    coll.explain("executionStats").aggregate(pipeline, {"allowDiskUse": true}), "$group");
checkGroupStages(groupStages, {}, true, true);

groupStages = getAggPlanStages(
    coll.explain("allPlansExecution").aggregate(pipeline, {"allowDiskUse": true}), "$group");
checkGroupStages(groupStages, {}, true, true);

groupStages = getAggPlanStages(
    coll.explain("queryPlanner").aggregate(pipeline, {"allowDiskUse": true}), "$group");
checkGroupStages(groupStages, {}, false, false);

MongoRunner.stopMongod(conn);
}());

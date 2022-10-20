/**
 * Tests that $group stage reports memory footprint per accumulator when explain is run with
 * verbosities "executionStats" and "allPlansExecution".
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStage().
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.

const conn = MongoRunner.runMongod();
const testDB = conn.getDB('test');
const coll = testDB.explain_group_stage_exec_stats;
coll.drop();

if (checkSBEEnabled(testDB)) {
    // When the SBE $group pushdown feature is enabled, a $group alone is pushed down and the
    // memory usage tracking isn't on a per-accumulator basis so this test is exercising
    // spilling behavior of the classic DocumentSourceGroup stage.
    jsTest.log("Skipping test since SBE $group pushdown has different memory tracking behavior");
    MongoRunner.stopMongod(conn);
    return;
}

const bigStr = Array(1025).toString();  // 1KB of ','
const maxMemoryLimitForGroupStage = 1024 * 300;
const debugBuild = testDB.adminCommand('buildInfo').debug;
const nDocs = 1000;
const nGroups = 50;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({_id: i, a: i, b: i % nGroups, bigStr: bigStr});
}
assert.commandWorked(bulk.execute());

const pipeline = [
    {$match: {a: {$gt: 0}}},
    {$sort: {b: 1}},
    {$group: {_id: "$b", count: {$sum: 1}, push: {$push: "$bigStr"}, set: {$addToSet: "$bigStr"}}},
];

const expectedAccumMemUsages = {
    count: nGroups * 60,
    push: nDocs * 1024,
    set: nGroups * 1024,
};

const expectedTotalMemoryUsage =
    Object.values(expectedAccumMemUsages).reduce((acc, val) => acc + val, 0);
const expectedSpillCount = Math.ceil(expectedTotalMemoryUsage / maxMemoryLimitForGroupStage);

/**
 * Checks that the execution stats in the explain output for a $group stage are as expected.
 * - 'stage' is an explain output of $group stage.
 * - 'expectedAccumMemUsages' is used to check the memory footprint stats for each accumulator.
 * - 'isExecExplain' indicates that the explain output is run with verbosity "executionStats" or
 * "allPlansExecution".
 * - 'expectedSpills' indicates how many times the data was spilled to disk when executing $group
 * stage.
 */
function checkGroupStages(stage, expectedAccumMemUsages, isExecExplain, expectedSpills) {
    // Tracks the memory usage per accumulator in total as 'stages' passed in could be the explain
    // output across a cluster.
    let totalAccumMemoryUsageBytes = 0;
    assert(stage.hasOwnProperty("$group"), stage);

    if (isExecExplain) {
        assert(stage.hasOwnProperty("maxAccumulatorMemoryUsageBytes"), stage);
        assert(stage.hasOwnProperty("spillFileSizeBytes"), stage);
        assert(stage.hasOwnProperty("numBytesSpilledEstimate"), stage);

        const maxAccmMemUsages = stage["maxAccumulatorMemoryUsageBytes"];
        for (const field of Object.keys(maxAccmMemUsages)) {
            totalAccumMemoryUsageBytes += maxAccmMemUsages[field];

            // Ensures that the expected accumulators are all included and the corresponding
            // memory usage is in a reasonable range. Note that in debug mode, data will be
            // spilled to disk every time we add a new value to a pre-existing group.
            if (!debugBuild && expectedAccumMemUsages.hasOwnProperty(field)) {
                assert.gt(maxAccmMemUsages[field], expectedAccumMemUsages[field]);
                assert.lt(maxAccmMemUsages[field], 5 * expectedAccumMemUsages[field]);
            }
        }

        const spillFileSizeBytes = stage["spillFileSizeBytes"];
        const numBytesSpilledEstimate = stage["numBytesSpilledEstimate"];
        if (stage.usedDisk) {
            // We cannot compute the size of the spill file, so assert that it is non-zero if we
            // have spilled.
            assert.gt(spillFileSizeBytes, 0, stage);

            // The number of bytes spilled, on the other hand, is at least as much as the
            // accumulator memory usage.
            assert.gt(numBytesSpilledEstimate, totalAccumMemoryUsageBytes);
        } else {
            assert.eq(spillFileSizeBytes, 0, stage);
            assert.eq(numBytesSpilledEstimate, 0, stage);
        }

        // Don't verify spill count for debug builds, since for debug builds a spill occurs on every
        // duplicate id in a group.
        if (!debugBuild) {
            assert.eq(stage.usedDisk, expectedSpills > 0, stage);
            assert.gte(stage.spills, expectedSpills, stage);
            assert.lte(stage.spills, 2 * expectedSpills, stage);
        }
    } else {
        assert(!stage.hasOwnProperty("usedDisk"), stage);
        assert(!stage.hasOwnProperty("spills"), stage);
        assert(!stage.hasOwnProperty("maxAccumulatorMemoryUsageBytes"), stage);
        assert(!stage.hasOwnProperty("spillFileSizeBytes"), stage);
        assert(!stage.hasOwnProperty("numBytesSpilledEstimate"), stage);
    }

    // Add some wiggle room to the total memory used compared to the limit parameter since the check
    // for spilling to disk happens after each document is processed.
    if (expectedSpills > 0)
        assert.gt(maxMemoryLimitForGroupStage + 4 * 1024, totalAccumMemoryUsageBytes, stage);
}

let groupStages = getAggPlanStage(coll.explain("executionStats").aggregate(pipeline), "$group");
checkGroupStages(groupStages, expectedAccumMemUsages, true, 0);

groupStages = getAggPlanStage(coll.explain("allPlansExecution").aggregate(pipeline), "$group");
checkGroupStages(groupStages, expectedAccumMemUsages, true, 0);

groupStages = getAggPlanStage(coll.explain("queryPlanner").aggregate(pipeline), "$group");
checkGroupStages(groupStages, {}, false, 0);

// Set MaxMemory low to force spill to disk.
assert.commandWorked(testDB.adminCommand(
    {setParameter: 1, ["internalDocumentSourceGroupMaxMemoryBytes"]: maxMemoryLimitForGroupStage}));

groupStages = getAggPlanStage(
    coll.explain("executionStats").aggregate(pipeline, {"allowDiskUse": true}), "$group");
checkGroupStages(groupStages, {}, true, expectedSpillCount);

groupStages = getAggPlanStage(
    coll.explain("allPlansExecution").aggregate(pipeline, {"allowDiskUse": true}), "$group");
checkGroupStages(groupStages, {}, true, expectedSpillCount);

groupStages = getAggPlanStage(
    coll.explain("queryPlanner").aggregate(pipeline, {"allowDiskUse": true}), "$group");
checkGroupStages(groupStages, {}, false, 0);

MongoRunner.stopMongod(conn);
}());

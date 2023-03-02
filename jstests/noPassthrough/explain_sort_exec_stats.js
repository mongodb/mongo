/**
 * Tests that $sort stage reports the correct stats when explain is run with
 * different verbosities.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStages().
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.explain_sort_stage_exec_stats;
coll.drop();

const isSbeEnabled = checkSBEEnabled(db);
const bigStr = Array(1025).toString();  // 1KB of ','
const lowMaxMemoryLimit = 5000;
const nDocs = 1000;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({_id: i, a: i, b: i, bigStr: bigStr});
}
assert.commandWorked(bulk.execute());

const pipelines = [
    {sortUsesDocumentSources: false, pipeline: [{$sort: {_id: 1, b: -1}}]},  // no limit
    {
        sortUsesDocumentSources: true,
        pipeline: [{$_internalInhibitOptimization: {}}, {$sort: {_id: 1, b: -1}}]
    },  // no limit document sources
    {
        sortUsesDocumentSources: false,
        pipeline: [{$sort: {_id: 1, b: -1}}, {$limit: nDocs / 10}]
    },  // top k sorter
    {
        sortUsesDocumentSources: true,
        pipeline:
            [{$_internalInhibitOptimization: {}}, {$sort: {_id: 1, b: -1}}, {$limit: nDocs / 10}]
    },  // top k sorter document sources
];

function checkSortSpillStats(explainOutput, shouldSpill, sortUsesDocumentSources) {
    let execStage = {};
    if (sortUsesDocumentSources) {
        execStage = getAggPlanStage(explainOutput, "$sort");
    } else if (isSbeEnabled) {
        execStage = getAggPlanStage(explainOutput, "sort");
    } else {
        execStage = getAggPlanStage(explainOutput, "SORT");
    }
    assert.neq(null, execStage, explainOutput);

    assert(execStage.hasOwnProperty("usedDisk"), execStage);
    assert(execStage.hasOwnProperty("spills"), execStage);
    assert(execStage.hasOwnProperty("spilledDataStorageSize"), execStage);

    const usedDisk = execStage.usedDisk;
    const spills = execStage.spills;
    const spilledDataStorageSize = execStage.spilledDataStorageSize;

    if (shouldSpill) {
        assert(usedDisk, explainOutput);
        assert.gt(spills, 0, explainOutput);
        assert.gt(spilledDataStorageSize, 0, explainOutput);
    } else {
        assert(!usedDisk, explainOutput);
        assert.eq(spills, 0, explainOutput);
        assert.eq(spilledDataStorageSize, 0, explainOutput);
    }
}

let explainOutput = {};

pipelines.forEach(function(pipeline) {
    // Set MaxMemory low to force spill to disk.
    const originalMemoryLimit = assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: lowMaxMemoryLimit}));

    explainOutput = coll.explain("executionStats").aggregate(pipeline.pipeline);
    checkSortSpillStats(explainOutput, true /*shouldSpill*/, pipeline.sortUsesDocumentSources);

    // Set MaxMemory to back to the original value.
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: originalMemoryLimit.was}));

    explainOutput = coll.explain("executionStats").aggregate(pipeline.pipeline);
    checkSortSpillStats(explainOutput, false /*shouldSpill*/, pipeline.sortUsesDocumentSources);
});

MongoRunner.stopMongod(conn);
}());

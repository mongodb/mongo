/**
 * Tests that $group stage reports spill stats when serverStatus is run.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStage().

const conn = MongoRunner.runMongod();
const db = conn.getDB('test');
const coll = db.explain_group_stage_exec_stats;
coll.drop();

const bigStr = Array(1025).toString();  // 1KB of ','
const maxMemoryLimitForGroupStage = 1024 * 300;
const nDocs = 1000;
const nGroups = 50;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({_id: i, a: i, b: i % nGroups, bigStr: bigStr});
}
assert.commandWorked(bulk.execute());

const pipeline = [
    {$_internalInhibitOptimization: {}},
    {$match: {a: {$gt: 0}}},
    {$sort: {b: 1}},
    {$group: {_id: "$b", count: {$sum: 1}, push: {$push: "$bigStr"}, set: {$addToSet: "$bigStr"}}},
];

const metricsBefore = db.serverStatus().metrics.query.group;

// Set MaxMemory low to force spill to disk.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: maxMemoryLimitForGroupStage}));

const result = getAggPlanStage(coll.explain("executionStats").aggregate(pipeline), "$group");

const metricsAfter = db.serverStatus().metrics.query.group;

const expectedSpills = result.spills + metricsBefore.spills;
const expectedSpillFileSizeBytes = result.spillFileSizeBytes + metricsBefore.spillFileSizeBytes;
const expectedNumBytesSpilledEstimate =
    result.numBytesSpilledEstimate + metricsBefore.numBytesSpilledEstimate;

assert.gt(metricsAfter.spills, metricsBefore.spills, pipeline);

assert.eq(metricsAfter.spills, expectedSpills, pipeline);

assert.gt(metricsAfter.spillFileSizeBytes, metricsBefore.spillFileSizeBytes, pipeline);

assert.eq(metricsAfter.spillFileSizeBytes, expectedSpillFileSizeBytes, pipeline);

assert.gt(metricsAfter.numBytesSpilledEstimate, metricsBefore.numBytesSpilledEstimate, pipeline);

assert.eq(metricsAfter.numBytesSpilledEstimate, expectedNumBytesSpilledEstimate, pipeline);

MongoRunner.stopMongod(conn);
}());

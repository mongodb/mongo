/**
 * Tests that $group stage reports spill stats when serverStatus is run.
 *
 * @tags: [
 *   # TODO SERVER-73757: Allow this test to run against the inMemory storage engine once ephemeral
 *   # temporary record stores used for spilling report the correct storage size.
 *   requires_persistence,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStage().
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.

const conn = MongoRunner.runMongod();
const db = conn.getDB('test');
const coll = db.explain_group_stage_exec_stats;
coll.drop();

const bigStr = Array(1025).toString();  // 1KB of ','
const maxMemoryLimitForGroupStage = 1024 * 300;
const nDocs = 1000;
const nGroups = 50;
const isSbeEnabled = checkSBEEnabled(db);

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

const metricsBefore = db.serverStatus().metrics.query.group;

// Set MaxMemory low to force spill to disk.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: maxMemoryLimitForGroupStage}));
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill:
        maxMemoryLimitForGroupStage
}));

const result = coll.explain("executionStats").aggregate(pipeline);

const groupStage =
    isSbeEnabled ? getAggPlanStage(result, "group") : getAggPlanStage(result, "$group");

const metricsAfter = db.serverStatus().metrics.query.group;

const expectedSpills = groupStage.spills;
const expectedSpilledDataStorageSize = groupStage.spilledDataStorageSize;
const expectedSpilledRecords = groupStage.spilledRecords;

assert.gt(metricsAfter.spills, metricsBefore.spills, pipeline);

assert.eq(metricsAfter.spills, expectedSpills + metricsBefore.spills, pipeline);

assert.gt(metricsAfter.spilledDataStorageSize, metricsBefore.spilledDataStorageSize, pipeline);

assert.eq(metricsAfter.spilledDataStorageSize,
          expectedSpilledDataStorageSize + metricsBefore.spilledDataStorageSize,
          pipeline);

assert.gt(metricsAfter.spilledRecords, metricsBefore.spilledRecords, pipeline);

assert.eq(
    metricsAfter.spilledRecords, expectedSpilledRecords + metricsBefore.spilledRecords, pipeline);

MongoRunner.stopMongod(conn);
}());

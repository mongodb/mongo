/**
 * Tests that block hashagg correctly reports spilling metrics.
 *
 * @tags: [
 *   # TODO SERVER-73757: Allow this test to run against the inMemory storage engine once ephemeral
 *   # temporary record stores used for spilling report the correct storage size.
 *   requires_persistence,
 * ]
 */
import {getAggPlanStage, getEngine} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod(
    {setParameter: {featureFlagSbeFull: true, featureFlagTimeSeriesInSbe: true}});
const db = conn.getDB('test');
// Debug mode changes how often we spill. This test assumes SBE is being used.
if (db.adminCommand('buildInfo').debug ||
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
            .internalQueryFrameworkControl == "forceClassicEngine") {
    jsTestLog("Returning early because debug or forceClassic is on.");
    MongoRunner.stopMongod(conn);
    quit();
}

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(db.createCollection(coll.getName(), {
    timeseries: {timeField: 't', metaField: 'm'},
}));

const bigStr = Array(1025).toString();  // 1KB of ','
// Around 300KB.
const maxMemoryLimitForGroupStage = 1024 * 300;
const nDocs = 1000;
const nGroups = 50;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({_id: i, t: new Date(), m: i, a: i, b: i % nGroups, bigStr: bigStr});
}
assert.commandWorked(bulk.execute());

// Grouping by $bigStr and $a (a unique field) produces enough keys in our hash table to go over the
// memory limit and spill.
const pipeline = [
    {$match: {a: {$gt: 0}}},
    {$group: {_id: {x: '$bigStr', y: '$a'}, min: {$min: "$b"}}},
];
const expectedMetrics = {
    spills: 4,
    spilledDataStorageSize: 4096,
    spilledRecords: 1000
};

// Test that the server status metrics before and after are correctly incremented.
const metricsBefore = db.serverStatus().metrics.query.group;

// Set MaxMemory low to force spill to disk.
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill:
        maxMemoryLimitForGroupStage
}));

const explain = coll.explain("executionStats").aggregate(pipeline);
assert.eq(getEngine(explain), "sbe");
const groupStage = getAggPlanStage(explain, "block_group");

const metricsAfter = db.serverStatus().metrics.query.group;

const actualSpills = groupStage.spills;
const actualSpilledDataStorageSize = groupStage.spilledDataStorageSize;
const actualSpilledRecords = groupStage.spilledRecords;

// Assert on the explain metrics.
assert(groupStage.usedDisk);
assert.eq(actualSpills, expectedMetrics.spills);
assert.eq(actualSpilledDataStorageSize, expectedMetrics.spilledDataStorageSize);
assert.eq(actualSpilledRecords, expectedMetrics.spilledRecords);

// Assert on the server status metrics.
assert.eq(metricsAfter.spills, actualSpills + metricsBefore.spills, explain);
assert.eq(metricsAfter.spilledDataStorageSize,
          actualSpilledDataStorageSize + metricsBefore.spilledDataStorageSize,
          explain);
assert.eq(metricsAfter.spilledDataStorageSize,
          actualSpilledDataStorageSize + metricsBefore.spilledDataStorageSize,
          explain);
assert.eq(
    metricsAfter.spilledRecords, actualSpilledRecords + metricsBefore.spilledRecords, explain);
assert.eq(
    metricsAfter.spilledRecords, actualSpilledRecords + metricsBefore.spilledRecords, explain);

MongoRunner.stopMongod(conn);

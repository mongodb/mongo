/**
 * Tests that block hashagg correctly reports block-specific metrics.
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

/*
 * We'll have 50 time series buckets (which become blocks in our block processing pipeline). 25 of
 * these buckets will have a small number of partitions (2, of which only 1 will be matching the
 * filter), enabling the use of block-based accumulators. The other 25 will have many partitions
 * (100), meaning we use the element-wise accumulators.
 */
const nBuckets = 50;
const nDocsPerBucket = 200;

const bulk = coll.initializeUnorderedBulkOp();
for (let m = 1; m <= nBuckets; m++) {
    // How many partitions are in this bucket? If m is even, we choose 2, otherwise 100.
    const nPartitions = m % 2 === 0 ? 2 : 100;
    for (let i = 0; i < nDocsPerBucket; i++) {
        bulk.insert({t: new Date(), m, a: i % nPartitions, b: i});
    }
}
assert.commandWorked(bulk.execute());

// Simple $group stage that will trigger block processing.
const pipeline = [
    {$match: {a: {$gt: 0}}},
    {$group: {_id: {y: '$a'}, min: {$min: "$b"}}},
];

const explain = coll.explain("executionStats").aggregate(pipeline);
assert.eq(getEngine(explain), "sbe");
const groupStage = getAggPlanStage(explain, "block_group");

assert.eq(groupStage.blockAccumulations, 25);
assert.eq(groupStage.blockAccumulatorTotalCalls, 25);
assert.eq(groupStage.elementWiseAccumulations, 25);

MongoRunner.stopMongod(conn);

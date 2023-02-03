/**
 * Tests the serverStatus and FTDC metrics for multi planner execution (both classic and SBE).
 */
(function() {
"use strict";

function sumHistogramBucketCounts(histogram) {
    let sum = 0;
    for (const bucket of histogram) {
        sum += bucket.count;
    }
    return sum;
}

load("jstests/libs/ftdc.js");
load("jstests/libs/sbe_util.js");  // For 'checkSBEEnabled()'.

const collName = jsTestName();
const dbName = jsTestName();

// Use an isolated server instance to obtain predictible serverStatus planning metrics.
const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(dbName);

// This test assumes that SBE is being used for most queries.
if (!checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTestLog("Skipping test because SBE is not fully enabled");
    MongoRunner.stopMongod(conn);
    return;
}

let coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insert({_id: 3, a: 1, b: 1}));
assert.commandWorked(coll.insert({_id: 5, a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

let multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;

// Verify initial metrics.
assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicMicros), 0);
assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicNumPlans), 0);
assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicWorks), 0);
assert.eq(multiPlannerMetrics.classicCount, 0);
assert.eq(multiPlannerMetrics.classicMicros, 0);
assert.eq(multiPlannerMetrics.classicWorks, 0);

assert.eq(multiPlannerMetrics.histograms.classicMicros[0].lowerBound, 0);

// Run with classic engine and verify metrics.
{
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());

    multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.sbeMicros), 0);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.sbeNumReads), 0);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.sbeNumPlans), 0);
    assert.eq(multiPlannerMetrics.sbeMicros, 0);
    assert.eq(multiPlannerMetrics.sbeNumReads, 0);
    assert.eq(multiPlannerMetrics.sbeCount, 0);

    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicMicros), 1);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicNumPlans), 1);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicWorks), 1);
    assert.eq(multiPlannerMetrics.classicCount, 1);
    assert.gt(multiPlannerMetrics.classicMicros, 0);
    assert.gt(multiPlannerMetrics.classicWorks, 0);
}

// Run with SBE and verify metrics.
{
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());

    multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.sbeMicros), 1);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.sbeNumReads), 1);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.sbeNumPlans), 1);
    assert.gt(multiPlannerMetrics.sbeMicros, 0);
    assert.gt(multiPlannerMetrics.sbeNumReads, 0);
    assert.eq(multiPlannerMetrics.sbeCount, 1);

    // Sanity check.
    assert.eq(multiPlannerMetrics.classicCount, 1);
}

assert.soon(() => {
    // Verify FTDC includes aggregate metrics.
    const multiPlannerMetricsFtdc =
        verifyGetDiagnosticData(conn.getDB("admin")).serverStatus.metrics.query.multiPlanner;
    if (multiPlannerMetricsFtdc.classicCount == 0 || multiPlannerMetricsFtdc.sbeCount == 0) {
        // This is an indication we haven't retrieve the expected serverStatus metrics yet.
        return false;
    }
    assert.eq(multiPlannerMetricsFtdc.classicCount, 1);
    assert.gt(multiPlannerMetricsFtdc.classicMicros, 0);
    assert.gt(multiPlannerMetricsFtdc.classicWorks, 0);
    assert.eq(multiPlannerMetricsFtdc.sbeCount, 1);
    assert.gt(multiPlannerMetricsFtdc.sbeMicros, 0);
    assert.gt(multiPlannerMetricsFtdc.sbeNumReads, 0);
    // Verify FTDC omits detailed histograms.
    assert(!multiPlannerMetricsFtdc.hasOwnProperty("histograms"));
    return true;
}, "FTDC output should eventually reflect observed serverStatus metrics.");

MongoRunner.stopMongod(conn);
}());

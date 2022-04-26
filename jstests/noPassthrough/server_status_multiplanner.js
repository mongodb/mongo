/**
 * Tests the serverStatus and FTDC metrics for multi planner execution.
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

const collName = jsTestName();
const dbName = jsTestName();

// Use an isolated server instance to obtain predictible serverStatus planning metrics.
const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(dbName);

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
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());

    multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;

    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicMicros), 1);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicNumPlans), 1);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicWorks), 1);
    assert.eq(multiPlannerMetrics.classicCount, 1);
    assert.gt(multiPlannerMetrics.classicMicros, 0);
    assert.gt(multiPlannerMetrics.classicWorks, 0);
}

assert.soon(() => {
    // Verify FTDC includes aggregate metrics.
    const multiPlannerMetricsFtdc =
        verifyGetDiagnosticData(conn.getDB("admin")).serverStatus.metrics.query.multiPlanner;
    if (multiPlannerMetricsFtdc.classicCount == 0) {
        // This is an indication we haven't retrieve the expected serverStatus metrics yet.
        return false;
    }
    assert.eq(multiPlannerMetricsFtdc.classicCount, 1);
    assert.gt(multiPlannerMetricsFtdc.classicMicros, 0);
    assert.gt(multiPlannerMetricsFtdc.classicWorks, 0);
    // Verify FTDC omits detailed histograms.
    assert(!multiPlannerMetricsFtdc.hasOwnProperty("histograms"));
    return true;
}, "FTDC output should eventually reflect observed serverStatus metrics.");

MongoRunner.stopMongod(conn);
}());

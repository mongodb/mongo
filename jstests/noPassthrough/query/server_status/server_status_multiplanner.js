/**
 * Tests the serverStatus and FTDC metrics for multi planner execution (both classic and SBE).
 */

function sumHistogramBucketCounts(histogram) {
    let sum = 0;
    for (const bucket of histogram) {
        sum += bucket.count;
    }
    return sum;
}

import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

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

function assertClassicMultiPlannerMetrics(multiPlannerMetrics, expectedCount) {
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicMicros),
              expectedCount);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicNumPlans),
              expectedCount);
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicWorks), expectedCount);
    assert.eq(multiPlannerMetrics.classicCount, expectedCount);
    assert.eq(multiPlannerMetrics.stoppingCondition.hitEof, expectedCount * 2);
    assert.eq(multiPlannerMetrics.stoppingCondition.hitResultsLimit, 0);
    assert.eq(multiPlannerMetrics.stoppingCondition.hitWorksLimit, 0);
    if (expectedCount > 0) {
        assert.gt(multiPlannerMetrics.classicMicros, 0);
        assert.gt(multiPlannerMetrics.classicWorks, 0);
        assert.gt(multiPlannerMetrics.classicNumPlans, expectedCount);
    } else {
        assert.eq(multiPlannerMetrics.classicMicros, 0);
        assert.eq(multiPlannerMetrics.classicWorks, 0);
        assert.eq(multiPlannerMetrics.classicNumPlans, 0);
    }
}

// Verify initial metrics.
let multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;
assertClassicMultiPlannerMetrics(multiPlannerMetrics, 0);

// Run with classic engine and verify metrics.
{
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());

    multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;
    assertClassicMultiPlannerMetrics(multiPlannerMetrics, 1);
}

// Run with SBE and verify metrics.
{
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());

    multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;
    assertClassicMultiPlannerMetrics(multiPlannerMetrics, 2);
}

assert.soon(() => {
    // Verify FTDC includes aggregate metrics.
    const multiPlannerMetricsFtdc =
        verifyGetDiagnosticData(conn.getDB("admin")).serverStatus.metrics.query.multiPlanner;

    const expectedClassicCount = 2;
    if (multiPlannerMetricsFtdc.classicCount != expectedClassicCount) {
        // This is an indication we haven't retrieve the expected serverStatus metrics yet.
        return false;
    }

    assertClassicMultiPlannerMetrics(multiPlannerMetricsFtdc, expectedClassicCount);
    return true;
}, "FTDC output should eventually reflect observed serverStatus metrics.");

// Test 'stoppingConditions.hitWorksLimit'.
{
    // Run the query with a low works limit.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 1}));
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 10000}));

    assert.docEq(db.serverStatus().metrics.query.multiPlanner.stoppingCondition, {
        hitEof: 4,
        hitWorksLimit: 1,
        hitResultsLimit: 0,
    });
}

// Test 'stoppingConditions.hitResultsLimit'.
{
    // Add two matching docs to avoid hitting EOF.
    assert.commandWorked(coll.insert({_id: 6, a: 1, b: 1, c: 1}));
    assert.commandWorked(coll.insert({_id: 7, a: 1, b: 1, c: 1}));

    // Run the query with a low results limit.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationMaxResults: 1}));
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationMaxResults: 101}));

    assert.docEq(db.serverStatus().metrics.query.multiPlanner.stoppingCondition, {
        hitEof: 4,
        hitWorksLimit: 1,
        hitResultsLimit: 2,
    });
}

MongoRunner.stopMongod(conn);

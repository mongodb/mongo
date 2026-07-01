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

// Use an isolated server instance to obtain predictible serverStatus planning metrics. Disable CBR, because it changes multi planner metrics.
// TODO SERVER-122264 Enable CBR for this test.
const conn = MongoRunner.runMongod({setParameter: {featureFlagCostBasedRanker: false}});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(dbName);
let coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insert({_id: 3, a: 1, b: 1}));
assert.commandWorked(coll.insert({_id: 5, a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

function assertClassicMultiPlannerMetrics(multiPlannerMetrics, expectedCount) {
    assert.eq(
        sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicMicros),
        expectedCount,
    );
    assert.eq(
        sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicNumPlans),
        expectedCount,
    );
    assert.eq(sumHistogramBucketCounts(multiPlannerMetrics.histograms.classicWorks), expectedCount);
    assert.eq(multiPlannerMetrics.classicCount, expectedCount);
    assert.eq(multiPlannerMetrics.choseWinningPlan, expectedCount);
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
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
    );
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());

    multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;
    assertClassicMultiPlannerMetrics(multiPlannerMetrics, 1);
}

// Run with SBE and verify metrics.
{
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}),
    );
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());

    multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;
    assertClassicMultiPlannerMetrics(multiPlannerMetrics, 2);
}

assert.soon(
    () => {
        // Verify FTDC includes aggregate metrics.
        const multiPlannerMetricsFtdc = verifyGetDiagnosticData(conn.getDB("admin")).serverStatus
            .metrics.query.multiPlanner;

        const expectedClassicCount = 2;
        if (multiPlannerMetricsFtdc.classicCount != expectedClassicCount) {
            // This is an indication we haven't retrieve the expected serverStatus metrics yet.
            return false;
        }

        try {
            assertClassicMultiPlannerMetrics(multiPlannerMetricsFtdc, expectedClassicCount);
        } catch (e) {
            // All counters are updated individually, so it is possible that classicCount is updated, but histograms are not.
            return false;
        }
        return true;
    },
    () =>
        "FTDC output should eventually reflect observed serverStatus metrics. Current FTDC: " +
        tojson(
            verifyGetDiagnosticData(conn.getDB("admin")).serverStatus.metrics.query.multiPlanner,
        ),
);

// Test 'stoppingConditions.hitWorksLimit'.
{
    // Run the query with a low works limit.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 1}));
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 10000}),
    );

    multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;
    assert.docEq(multiPlannerMetrics.stoppingCondition, {
        hitEof: 4,
        hitWorksLimit: 1,
        hitResultsLimit: 0,
    });
    assert.eq(multiPlannerMetrics.choseWinningPlan, 3);
}

// Test 'stoppingConditions.hitResultsLimit'.
{
    // Add two matching docs to avoid hitting EOF.
    assert.commandWorked(coll.insert({_id: 6, a: 1, b: 1, c: 1}));
    assert.commandWorked(coll.insert({_id: 7, a: 1, b: 1, c: 1}));

    // Run the query with a low results limit.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationMaxResults: 1}),
    );
    assert.commandWorked(coll.find({a: 1, b: 1, c: 1}).explain());
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationMaxResults: 101}),
    );

    multiPlannerMetrics = db.serverStatus().metrics.query.multiPlanner;
    assert.docEq(multiPlannerMetrics.stoppingCondition, {
        hitEof: 4,
        hitWorksLimit: 1,
        hitResultsLimit: 2,
    });
    assert.eq(multiPlannerMetrics.choseWinningPlan, 4);
}

// Test 'switchedToBackupPlan'. The backup plan switch happens when the winning plan (a blocking
// AND_SORTED intersection wrapped in a SORT) exceeds the memory limit during execution and a
// non-blocking backup plan is available.
{
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
    );

    const backupColl = db.getCollection("backup_plan_test");
    backupColl.drop();

    // Insert enough data so the blocking sort exceeds the memory limit during full execution,
    // but not during the multi-planner trial period (which only processes a fraction of docs).
    const numDocs = 1000;
    const docSize = 1024;
    const bulk = backupColl.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({a: 1, b: 1, pad: "x".repeat(docSize)});
    }
    assert.commandWorked(bulk.execute());

    assert.commandWorked(backupColl.createIndex({a: 1}));
    assert.commandWorked(backupColl.createIndex({b: 1}));

    // Force index intersection so the AND_SORTED plan (which includes a blocking sort) wins
    // multi-planning. A non-blocking single-index plan becomes the backup.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: true}),
    );
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerEnableSortIndexIntersection: true}),
    );

    // The sort memory limit must be large enough for the trial period to succeed (so the
    // blocking plan wins and a backup is set), but small enough that full execution OOMs.
    // The trial processes roughly 0.29 * numDocs works. Set the limit to hold ~half the docs.
    const origSortBytes = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1}),
    ).internalQueryMaxBlockingSortMemoryUsageBytes;
    const sortLimit = (numDocs * docSize) / 2;
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: sortLimit}),
    );

    backupColl.getPlanCache().clear();

    const metricsBefore = db.serverStatus().metrics.query.multiPlanner.switchedToBackupPlan;

    // allowDiskUse:false so the sort throws QueryExceededMemoryLimitNoDiskUseAllowed instead of
    // spilling, triggering the switch to the backup plan.
    const cmdResult = db.runCommand({
        find: backupColl.getName(),
        filter: {a: 1, b: 1},
        sort: {b: 1},
        allowDiskUse: false,
    });
    assert.commandWorked(cmdResult);
    const results = new DBCommandCursor(db, cmdResult).toArray();
    assert.eq(numDocs, results.length);

    const metricsAfter = db.serverStatus().metrics.query.multiPlanner.switchedToBackupPlan;
    assert.gt(
        metricsAfter,
        metricsBefore,
        "Expected switchedToBackupPlan to increment; before=" +
            metricsBefore +
            " after=" +
            metricsAfter,
    );

    // Restore parameters.
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryMaxBlockingSortMemoryUsageBytes: origSortBytes,
        }),
    );
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: false}),
    );
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerEnableSortIndexIntersection: false}),
    );
}

MongoRunner.stopMongod(conn);

/**
 * Tests the serverStatus and FTDC metrics for CBR (Cost-Based Ranking) plan selection.
 *
 * Verifies all query.cbr.* metrics under samplingCE mode.
 *
 * @tags: [requires_fcv_83]
 */

// TODO (SERVER-121763): Move all server status tests to noPassthroughWithMongod

import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

function sumHistogramBucketCounts(histogram) {
    let sum = 0;
    for (const [_, bucket] of Object.entries(histogram)) {
        if (bucket.hasOwnProperty("count")) {
            sum += bucket.count;
        }
    }
    return sum;
}

const collName = jsTestName();
const dbName = jsTestName();

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(dbName);
let coll = db.getCollection(collName);
coll.drop();

// Enable CBR with samplingCE mode.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        featureFlagCostBasedRanker: true,
        internalQueryCBRCEMode: "samplingCE",
        internalQuerySamplingBySequentialScan: true,
    }),
);

// Force the classic engine so CBR applies.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

// Insert enough documents for sampling to work.
const kNumDocs = 1000;
const docs = [];
for (let i = 0; i < kNumDocs; i++) {
    docs.push({a: i, b: i, c: i % 10});
}
assert.commandWorked(coll.insertMany(docs));

// Create two indexes to force multi-planning (CBR needs >= 2 candidate plans).
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

function getCBRMetrics() {
    return db.serverStatus().metrics.query.cbr;
}

function assertCBRCounterMetrics(cbrMetrics, expectedCount) {
    assert.eq(
        cbrMetrics.count,
        expectedCount,
        `cbr.count mismatch. Expected value: ${expectedCount} Current value: ${cbrMetrics.count}`,
    );
    if (expectedCount > 0) {
        assert.gt(cbrMetrics.micros, 0, `cbr.micros should be > 0:\n${cbrMetrics}`);
        assert.gt(cbrMetrics.samplingMicros, 0, "cbr.samplingMicros should be > 0:\n${cbrMetrics}");
        assert.gte(
            cbrMetrics.numPlans,
            expectedCount * 2,
            "cbr.numPlans should be >= 2 per invocation:\n${cbrMetrics}",
        );
    } else {
        assert.eq(
            cbrMetrics.micros,
            0,
            `cbr.micros should be 0. Expected value: 0 Current value: ${cbrMetrics.micros}`,
        );
        assert.eq(
            cbrMetrics.samplingMicros,
            0,
            `cbr.samplingMicros should be 0. Expected value: 0 Current value: ${cbrMetrics.samplingMicros}`,
        );
        assert.eq(
            cbrMetrics.numPlans,
            0,
            `cbr.numPlans should be 0. Expected value: 0 Current value: ${cbrMetrics.numPlans}`,
        );
    }
}

function assertCBRHistogramMetrics(cbrMetrics, expectedCount) {
    const microsCount = sumHistogramBucketCounts(cbrMetrics.histograms.micros);
    const samplingMicrosCount = sumHistogramBucketCounts(cbrMetrics.histograms.samplingMicros);
    const numPlansCount = sumHistogramBucketCounts(cbrMetrics.histograms.numPlans);
    assert.eq(
        microsCount,
        expectedCount,
        `cbr.histograms.micros bucket count mismatch. Expected value: ${expectedCount} Current value: ${microsCount}\n${tojson(cbrMetrics.histograms)}`,
    );
    assert.eq(
        samplingMicrosCount,
        expectedCount,
        `cbr.histograms.samplingMicros bucket count mismatch. Expected value: ${expectedCount} Current value: ${samplingMicrosCount}\n${tojson(cbrMetrics.histograms)}`,
    );
    assert.eq(
        numPlansCount,
        expectedCount,
        `cbr.histograms.numPlans bucket count mismatch. Expected value: ${expectedCount} Current value: ${numPlansCount}\n${tojson(cbrMetrics.histograms)}`,
    );
}

function getMultiPlannerMetrics() {
    return db.serverStatus().metrics.query.multiPlanner;
}

function getPlanningMetrics() {
    return db.serverStatus().metrics.query.planning;
}

function assertMultiPlannerMetricsUnchanged(before, after) {
    assert.eq(
        before.classicCount,
        after.classicCount,
        `multiPlanner.classicCount should not have changed. Previous value: ${before.classicCount} Current value: ${after.classicCount}`,
    );
    assert.eq(
        before.choseWinningPlan,
        after.choseWinningPlan,
        `multiPlanner.choseWinningPlan should not have changed. Previous value: ${before.choseWinningPlan} Current value: ${after.choseWinningPlan}`,
    );
}

// ======================
// samplingCE mode tests.
// ======================

const nonProductiveFilterWithMultipleSolutions = {nonexistentField: {$exists: true}, a: 1, b: 1};

// Verify all CBR metrics start at zero.
{
    const cbrMetrics = getCBRMetrics();
    assertCBRCounterMetrics(cbrMetrics, 0);
    assertCBRHistogramMetrics(cbrMetrics, 0);
    assert.eq(
        cbrMetrics.choseWinningPlan,
        0,
        `cbr.choseWinningPlan should start at 0. Expected value: 0 Current value: ${cbrMetrics.choseWinningPlan}`,
    );
    assert.eq(
        cbrMetrics.numPlansFailedCostEstimation,
        0,
        `cbr.numPlansFailedCostEstimation should start at 0. Expected value: 0 Current value: ${cbrMetrics.numPlansFailedCostEstimation}`,
    );
    assert.eq(
        cbrMetrics.numPlansTiedCostEstimation,
        0,
        `cbr.numPlansTiedCostEstimation should start at 0. Expected value: 0 Current value: ${cbrMetrics.numPlansTiedCostEstimation}`,
    );
}

// Run a query (via explain to avoid plan cache effects).
{
    const planningBefore = getPlanningMetrics();
    const mpBefore = getMultiPlannerMetrics();

    assert.commandWorked(coll.find(nonProductiveFilterWithMultipleSolutions).explain());
    const planningAfter = getPlanningMetrics();

    const cbrMetrics = getCBRMetrics();
    assertCBRCounterMetrics(cbrMetrics, 1);
    assertCBRHistogramMetrics(cbrMetrics, 1);

    assert.eq(
        cbrMetrics.choseWinningPlan,
        1,
        `cbr.choseWinningPlan should be 1 after first CBR invocation. Expected value: 1 Current value: ${cbrMetrics.choseWinningPlan}`,
    );

    assert.eq(
        planningAfter.invocationCount,
        planningBefore.invocationCount + 1,
        `planning.invocationCount should increase by 1 after first CBR explain. Previous value: ${planningBefore.invocationCount} Current value: ${planningAfter.invocationCount}`,
    );

    // In samplingCE mode, no MultiPlanStage is used, so multiplanner metrics stay unchanged.
    assertMultiPlannerMetricsUnchanged(mpBefore, getMultiPlannerMetrics());
}

// Run a second query to verify metrics accumulate.
{
    const planningBefore = getPlanningMetrics();
    const mpBefore = getMultiPlannerMetrics();

    assert.commandWorked(coll.find(nonProductiveFilterWithMultipleSolutions).explain());
    const planningAfter = getPlanningMetrics();

    const cbrMetrics = getCBRMetrics();
    assertCBRCounterMetrics(cbrMetrics, 2);
    assertCBRHistogramMetrics(cbrMetrics, 2);

    assert.eq(
        cbrMetrics.choseWinningPlan,
        2,
        `cbr.choseWinningPlan should be 2 after second CBR invocation. Expected value: 2 Current value: ${cbrMetrics.choseWinningPlan}`,
    );

    assert.eq(
        planningAfter.invocationCount,
        planningBefore.invocationCount + 1,
        `planning.invocationCount should increase by 1 after second CBR explain. Previous value: ${planningBefore.invocationCount} Current value: ${planningAfter.invocationCount}`,
    );

    assertMultiPlannerMetricsUnchanged(mpBefore, getMultiPlannerMetrics());
}

// Run non-explain queries to populate the plan cache, then verify that cached execution does NOT increment CBR or multiplanner metrics.
{
    coll.getPlanCache().clear();

    // First execution: creates inactive cache entry.
    coll.find(nonProductiveFilterWithMultipleSolutions).toArray();

    const cbrAfterFirst = getCBRMetrics();
    assert.eq(
        cbrAfterFirst.count,
        3,
        `cbr.count should be 3 after third invocation. Expected value: 3 Current value: ${cbrAfterFirst.count}`,
    );

    // Second execution: will activate cache entry for next run.
    coll.find(nonProductiveFilterWithMultipleSolutions).toArray();
    const cbrAfterSecond = getCBRMetrics();
    assert.eq(
        4,
        cbrAfterSecond.count,
        `cbr.count should increase by 1 after second execution. Previous value: ${cbrAfterFirst.count} Current value: ${cbrAfterSecond.count}`,
    );

    // Third execution: uses the active cache entry. Neither CBR nor multiplanner should run.
    const mpAfterSecond = getMultiPlannerMetrics();
    const planningBeforeThird = getPlanningMetrics();
    coll.find(nonProductiveFilterWithMultipleSolutions).toArray();
    const planningAfterThird = getPlanningMetrics();
    const cbrAfterThird = getCBRMetrics();

    assert.eq(
        4,
        cbrAfterThird.count,
        `cbr.count should not change on cached plan execution. Previous value: ${cbrAfterSecond.count} Current value: ${cbrAfterThird.count}`,
    );
    assert.eq(
        planningAfterThird.invocationCount,
        planningBeforeThird.invocationCount,
        `planning.invocationCount should not change on cached plan execution. Previous value: ${planningBeforeThird.invocationCount} Current value: ${planningAfterThird.invocationCount}`,
    );

    assertMultiPlannerMetricsUnchanged(mpAfterSecond, getMultiPlannerMetrics());
}

// Test: CBR metrics are available in FTDC
{
    const expectedCount = Number(getCBRMetrics().count);

    assert.soon(
        () => {
            // Verify FTDC includes CBR metrics.
            const cbrMetricsFtdc = verifyGetDiagnosticData(conn.getDB("admin")).serverStatus.metrics.query.cbr;

            if (cbrMetricsFtdc.count != expectedCount) {
                // This is an indication we haven't retrieved the expected serverStatus metrics yet.
                return false;
            }

            try {
                assertCBRCounterMetrics(cbrMetricsFtdc, expectedCount);
                assertCBRHistogramMetrics(cbrMetricsFtdc, expectedCount);
            } catch (e) {
                // All counters are updated individually, so it is possible that classicCount is updated, but histograms are not.
                return false;
            }
            return true;
        },
        () =>
            "FTDC output should eventually reflect observed serverStatus metrics. Current FTDC: " +
            tojson(verifyGetDiagnosticData(conn.getDB("admin")).serverStatus.metrics.query.cbr),
    );
}

MongoRunner.stopMongod(conn);

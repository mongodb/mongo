/**
 * Tests the serverStatus metrics for AutomaticCE plan selection under the
 * EstimateRankingEffort strategy.
 *
 * Verifies all query.cbr.* and query.multiPlanner.* metrics under mixed plan ranking mode with the
 * EstimateRankingEffort strategy. This strategy always
 * runs a brief multiplanner estimation trial, then uses a cost model to decide between MP and CBR:
 *
 *   MP wins  – estimation trial exits early (EOF/full batch) or CBR offers no clear improvement.
 *   CBR wins – estimation trial finds very low productivity; fall-back to CBR is more efficient.
 *
 * @tags: [requires_fcv_83]
 */

// TODO (SERVER-121763): Move all server status tests to noPassthroughWithMongod

import {
    assertCBRChoseWinner,
    assertCBRDidNotRun,
    assertMPChoseWinner,
    assertOneHistogramObservation,
    assertOnePlanningInvocation,
    getCBRMetrics,
    getMultiPlannerMetrics,
    getPlanningMetrics,
} from "jstests/libs/query/planning_metrics_utils.js";

const collName = jsTestName();
const dbName = jsTestName();

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(dbName);
let coll = db.getCollection(collName);
coll.drop();

// Make sure CBR is enabled in mixed plan ranking mode with the EstimateRankingEffort strategy.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        featureFlagCostBasedRanker: true,
        internalQueryPlanRanker: "mixed",
        internalQueryCBRCEMode: "samplingCE",
        internalQuerySamplingBySequentialScan: true,
        internalQueryMixedPlanRankingStrategy: "EstimateRankingEffort",
    }),
);

// Force the classic engine so CBR applies.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
);

// Insert enough documents for sampling to work.
const kNumDocs = 1000;
const docs = [];
for (let i = 0; i < kNumDocs; i++) {
    docs.push({a: i, b: i, c: i % 10});
}
assert.commandWorked(coll.insertMany(docs));

// Create two indexes to force multi-planning.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// ---------------------------------------------------------------------------
// MP wins: estimation trial exits early (case 1 — earlyExit).
// The query matches all 1000 docs, so both index plans fill a batch and MP picks a winner.
// ---------------------------------------------------------------------------
{
    coll.getPlanCache().clear();

    const cbrBefore = getCBRMetrics(db);
    const mpBefore = getMultiPlannerMetrics(db);
    const planningBefore = getPlanningMetrics(db);

    // Matches all 1000 docs; both plans on {a: 1} and {b: 1} fill a batch during estimation.
    assert.commandWorked(coll.find({a: {$gte: 0}, b: {$gte: 0}}).explain());

    const cbrAfter = getCBRMetrics(db);
    const mpAfter = getMultiPlannerMetrics(db);
    const planningAfter = getPlanningMetrics(db);

    // CBR should NOT have been invoked.
    assertCBRDidNotRun(cbrBefore, cbrAfter);

    // MP chose the winner during the estimation trial.
    assertMPChoseWinner(mpBefore, mpAfter);

    // Each histogram records exactly one accumulated observation.
    assertOneHistogramObservation(mpBefore, mpAfter);

    // Planning should have been invoked once for the explain.
    assertOnePlanningInvocation(planningBefore, planningAfter);
}

// ---------------------------------------------------------------------------
// MP wins: cost model prefers MP over CBR. The estimation trial finds results (moderate
// productivity) but does not reach EOF or fill in a full batch.
// The cost model compares MP vs CBR and concludes MP is cheaper.
// ---------------------------------------------------------------------------
{
    coll.getPlanCache().clear();

    const cbrBefore = getCBRMetrics(db);
    const mpBefore = getMultiPlannerMetrics(db);
    const planningBefore = getPlanningMetrics(db);

    // Matches 10% of documents; index plans find results during estimation but do not hit EOF.
    assert.commandWorked(coll.find({a: {$gte: 0}, b: {$gte: 0}, c: 5}).explain());

    const cbrAfter = getCBRMetrics(db);
    const mpAfter = getMultiPlannerMetrics(db);
    const planningAfter = getPlanningMetrics(db);

    // CBR should NOT have been invoked — cost model chose MP.
    assertCBRDidNotRun(cbrBefore, cbrAfter);

    // MP chose the winner; metrics are emitted once per planning invocation.
    assertMPChoseWinner(mpBefore, mpAfter);

    // Each histogram records exactly one accumulated observation.
    assertOneHistogramObservation(mpBefore, mpAfter);

    // Planning should have been invoked once for the explain.
    assertOnePlanningInvocation(planningBefore, planningAfter);
}

// ---------------------------------------------------------------------------
// CBR wins: very low plan productivity (case 3).
// The query matches no documents, so the cost model picks CBR.
// ---------------------------------------------------------------------------
{
    coll.getPlanCache().clear();

    const cbrBefore = getCBRMetrics(db);
    const mpBefore = getMultiPlannerMetrics(db);
    const planningBefore = getPlanningMetrics(db);

    // Zero-result query: the nonexistentField filter eliminates every document. All plans have low productivity.
    assert.commandWorked(
        coll.find({a: {$gte: 0}, b: {$gte: 0}, nonexistentField: {$exists: true}}).explain(),
    );

    const cbrAfter = getCBRMetrics(db);
    const mpAfter = getMultiPlannerMetrics(db);
    const planningAfter = getPlanningMetrics(db);

    // CBR should have been invoked and chosen the winning plan.
    assertCBRChoseWinner(cbrBefore, cbrAfter);

    // The estimation trial always runs; but MP did not choose the winner.
    assert.eq(
        mpAfter.classicCount,
        mpBefore.classicCount + 1,
        `multiPlanner.classicCount should increase by 1 for the estimation trial. Previous value: ${mpBefore.classicCount} Current value: ${mpAfter.classicCount}`,
    );
    assert.eq(
        mpAfter.choseWinningPlan,
        mpBefore.choseWinningPlan,
        `multiPlanner.choseWinningPlan should not change when CBR wins. Previous value: ${mpBefore.choseWinningPlan} Current value: ${mpAfter.choseWinningPlan}`,
    );

    // Each histogram records exactly one accumulated observation.
    assertOneHistogramObservation(mpBefore, mpAfter);

    // Planning should have been invoked once for the explain.
    assertOnePlanningInvocation(planningBefore, planningAfter);
}

MongoRunner.stopMongod(conn);

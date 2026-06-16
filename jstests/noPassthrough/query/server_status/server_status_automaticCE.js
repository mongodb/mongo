/**
 * Tests the serverStatus metrics for AutomaticCE plan selection with CBRForNoMPResultsStrategy.
 *
 * Verifies all query.cbr.* and query.multiPlanner.* metrics under automaticCE mode. There are
 * three possible planning paths in CBRForNoMPResultsStrategy:
 *
 *   Path 1a (MP wins)          – multiplanner reaches EOF during the capped trial phase.
 *   Path 1b (MP wins)          – multiplanner finds results during the capped trial phase and
 *                                continues with finish-up phase.
 *   Path 2  (CBR wins)         – no results in capped trial; CBR picks a single winner.
 *   Path 3  (CBR can't decide) – no results in capped trial; CBR returns multiple solutions
 *                                (at least one plan is uncostable), MP picks from that subset.
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

// Make sure CBR is enabled in automaticCE mode
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        featureFlagCostBasedRanker: true,
        internalQueryCBRCEMode: "automaticCE",
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

// ========================
// automaticCE mode tests.
// ========================

coll.getPlanCache().clear();

const nonProductiveFilterWithMultipleSolutions = {nonexistentField: {$exists: true}, a: 1, b: 1};

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryCBRCEMode: "automaticCE"}));

// ---------------------------------------------------------------------------
// Path 1a: multiplanner wins (results found during capped trials).
// With the default internalQueryPlanEvaluationWorks (10000), the capped trial period is large
// enough for plans to find results, so multiplanner picks the winning plan directly.
// ---------------------------------------------------------------------------
{
    const cbrBefore = getCBRMetrics(db);
    const mpBefore = getMultiPlannerMetrics(db);
    const planningBefore = getPlanningMetrics(db);

    assert.commandWorked(coll.find(nonProductiveFilterWithMultipleSolutions).explain());

    const cbrAfter = getCBRMetrics(db);
    const mpAfter = getMultiPlannerMetrics(db);
    const planningAfter = getPlanningMetrics(db);

    // CBR should NOT have been invoked since multiplanner hits EOF during capped trials.
    assertCBRDidNotRun(cbrBefore, cbrAfter);

    // Multiplanner trial metrics should be incremented, and multiplanner chose the winning plan.
    assertMPChoseWinner(mpBefore, mpAfter);

    // Each histogram should record exactly one new observation.
    assertOneHistogramObservation(mpBefore, mpAfter);

    // Planning should have been invoked once for the explain.
    assertOnePlanningInvocation(planningBefore, planningAfter);
}

// ---------------------------------------------------------------------------
// Path 1b (finish-up variant): MP chooses the winning plan after a second trials run.
// Set internalQueryPlanEvaluationWorks to 100 so that each plan gets ~50 works during
// the capped trial — well short of EOF on 1000 docs, but enough to produce results
// for a broad filter. Since the plans are productive but do not fill a batch/hitEOF, MP starts a second
// finish-up run without invoking CBR.
// ---------------------------------------------------------------------------
{
    const originalWorks = assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 100}),
    ).was;

    try {
        coll.getPlanCache().clear();

        const cbrBefore = getCBRMetrics(db);
        const mpBefore = getMultiPlannerMetrics(db);
        const planningBefore = getPlanningMetrics(db);

        // Matches 100 docs; plans on {a:1} and {b:1} both find results within the
        // first ~50 works, making them productive. MP finishes up without calling CBR.
        assert.commandWorked(coll.find({a: {$lt: 100}, b: {$lt: 100}}).explain());

        const cbrAfter = getCBRMetrics(db);
        const mpAfter = getMultiPlannerMetrics(db);
        const planningAfter = getPlanningMetrics(db);

        // CBR should NOT have been invoked since plans were productive.
        assertCBRDidNotRun(cbrBefore, cbrAfter);

        // Stats from both the capped and finish-up runs are accumulated and emitted once.
        assertMPChoseWinner(mpBefore, mpAfter);

        // Each histogram records exactly one accumulated observation spanning both runs.
        assertOneHistogramObservation(mpBefore, mpAfter);

        // Planning should have been invoked once for the explain.
        assertOnePlanningInvocation(planningBefore, planningAfter);
    } finally {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: originalWorks}));
    }
}

// ---------------------------------------------------------------------------
// Path 2: CBR chooses a plan (no results during capped trials).
// Set internalQueryPlanEvaluationWorks to 1, so each plan gets 0 works during the capped trial.
// The capped trial produces no results and does not exit early, triggering CBR.
// ---------------------------------------------------------------------------
{
    const originalKnobValue = assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 1}),
    ).was;

    try {
        coll.getPlanCache().clear();

        const cbrBefore = getCBRMetrics(db);
        const mpBefore = getMultiPlannerMetrics(db);
        const planningBefore = getPlanningMetrics(db);

        assert.commandWorked(coll.find(nonProductiveFilterWithMultipleSolutions).explain());

        const cbrAfter = getCBRMetrics(db);
        const mpAfter = getMultiPlannerMetrics(db);
        const planningAfter = getPlanningMetrics(db);

        // CBR should have been invoked and chosen the winning plan.
        assertCBRChoseWinner(cbrBefore, cbrAfter);

        // Multiplanner trial metrics should still be incremented once (the initial capped trial
        // ran, and the finishing-up trial emits accumulated stats exactly once).
        assert.eq(
            mpAfter.classicCount,
            mpBefore.classicCount + 1,
            `multiPlanner.classicCount should increase by 1 for the capped trial. Previous value: ${mpBefore.classicCount} Current value: ${mpAfter.classicCount}`,
        );

        // But multiPlanner.choseWinningPlan should NOT increase since CBR chose the winner.
        assert.eq(
            mpAfter.choseWinningPlan,
            mpBefore.choseWinningPlan,
            `multiPlanner.choseWinningPlan should not change when CBR wins. Previous value: ${mpBefore.choseWinningPlan} Current value: ${mpAfter.choseWinningPlan}`,
        );

        // Each histogram should record exactly one new observation that reflects only
        // the capped-phase stats.
        assertOneHistogramObservation(mpBefore, mpAfter);

        // Planning should have been invoked once for the explain (capped trials then CBR).
        assertOnePlanningInvocation(planningBefore, planningAfter);
    } finally {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: originalKnobValue}));
    }
}

// ---------------------------------------------------------------------------
// Path 3: CBR can't decide (no results in capped trial; CBR returns multiple solutions).
//
// Create a partial index {c:1} with partialFilterExpression:{c:{$gte:0}}, which is uncostable by
// CBR's CE, so CBR returns multiple solutions and resumes multiplanning.
// InternalQueryPlanEvaluationWorks is set low so the capped trial stays well below EOF.
// ---------------------------------------------------------------------------
{
    // Partial index: CBR cannot estimate the cost of plans using this index.
    assert.commandWorked(coll.createIndex({c: 1}, {partialFilterExpression: {c: {$gt: 0}}}));

    // Keep the capped-trial work count low enough that no plan scans to EOF (1000 docs > 33
    // works per plan ( 100 works / 3 plans).
    const originalWorks = assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 100}),
    ).was;

    try {
        coll.getPlanCache().clear();

        const cbrBefore = getCBRMetrics(db);
        const mpBefore = getMultiPlannerMetrics(db);
        const planningBefore = getPlanningMetrics(db);

        // Query matches no documents (no doc has field d == 0), so the capped trial produces no
        // results and no plan exits early, triggering the CBR fallback.
        assert.commandWorked(coll.find({a: {$gt: 0}, b: {$gt: 0}, c: {$gt: 0}, d: 0}).explain());

        const cbrAfter = getCBRMetrics(db);
        const mpAfter = getMultiPlannerMetrics(db);
        const planningAfter = getPlanningMetrics(db);

        // CBR was invoked (it tried to rank the plans) but could not choose a single winner
        // because the partial-index plan is uncostable.
        assert.eq(
            cbrAfter.count,
            cbrBefore.count + 1,
            `cbr.count should increase by 1 when CBR is invoked but cannot decide. Previous: ${cbrBefore.count} Current: ${cbrAfter.count}`,
        );
        assert.eq(
            cbrAfter.choseWinningPlan,
            cbrBefore.choseWinningPlan,
            `cbr.choseWinningPlan should not change when CBR cannot decide. Previous: ${cbrBefore.choseWinningPlan} Current: ${cbrAfter.choseWinningPlan}`,
        );

        // Multiplanner ran and ultimately picked the winner from the CBR-narrowed candidate set.
        assertMPChoseWinner(mpBefore, mpAfter);

        // Each histogram records exactly one observation combining both trial phases.
        assertOneHistogramObservation(mpBefore, mpAfter);

        // Planning was invoked once for the explain.
        assertOnePlanningInvocation(planningBefore, planningAfter);
    } finally {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: originalWorks}));
    }
}

MongoRunner.stopMongod(conn);

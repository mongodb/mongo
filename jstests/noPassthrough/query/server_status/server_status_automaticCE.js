/**
 * Tests the serverStatus metrics for CBR (Cost-Based Ranking) plan selection.
 *
 * Verifies all query.cbr.* metrics under automaticCE mode.
 *
 * @tags: [requires_fcv_83]
 */

// TODO (SERVER-121763): Move all server status tests to noPassthroughWithMongod

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

function getCBRMetrics() {
    return db.serverStatus().metrics.query.cbr;
}

function getMultiPlannerMetrics() {
    return db.serverStatus().metrics.query.multiPlanner;
}

function getPlanningMetrics() {
    return db.serverStatus().metrics.query.planning;
}

// ========================
// automaticCE mode tests.
// ========================

coll.getPlanCache().clear();

const nonProductiveFilterWithMultipleSolutions = {nonexistentField: {$exists: true}, a: 1, b: 1};

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryCBRCEMode: "automaticCE"}));

// Test: multiplanner wins (results found during capped trials).
// With the default internalQueryPlanEvaluationWorks (10000), the capped trial period is large
// enough for plans to find results, so multiplanner picks the winning plan directly.
{
    const cbrBefore = getCBRMetrics();
    const mpBefore = getMultiPlannerMetrics();
    const planningBefore = getPlanningMetrics();

    assert.commandWorked(coll.find(nonProductiveFilterWithMultipleSolutions).explain());

    const cbrAfter = getCBRMetrics();
    const mpAfter = getMultiPlannerMetrics();
    const planningAfter = getPlanningMetrics();

    // CBR should NOT have been invoked since multiplanner found results during capped trials.
    assert.eq(
        cbrAfter.count,
        cbrBefore.count,
        `cbr.count should not change when multiplanner wins. Previous value: ${cbrBefore.count} Current value: ${cbrAfter.count}`,
    );
    assert.eq(
        cbrAfter.choseWinningPlan,
        cbrBefore.choseWinningPlan,
        `cbr.choseWinningPlan should not change when multiplanner wins. Previous value: ${cbrBefore.choseWinningPlan} Current value: ${cbrAfter.choseWinningPlan}`,
    );

    // Multiplanner trial metrics should be incremented.
    assert.eq(
        mpAfter.classicCount,
        mpBefore.classicCount + 1,
        `multiPlanner.classicCount should increase when multiplanner runs. Previous value: ${mpBefore.classicCount} Current value: ${mpAfter.classicCount}`,
    );

    // Multiplanner chose the winning plan.
    assert.eq(
        mpAfter.choseWinningPlan,
        mpBefore.choseWinningPlan + 1,
        `multiPlanner.choseWinningPlan should increase by 1. Previous value: ${mpBefore.choseWinningPlan} Current value: ${mpAfter.choseWinningPlan}`,
    );

    // Planning should have been invoked once for the explain.
    assert.eq(
        planningAfter.invocationCount,
        planningBefore.invocationCount + 1,
        `planning.invocationCount should increase by 1 when multiplanner runs. Previous value: ${planningBefore.invocationCount} Current value: ${planningAfter.invocationCount}`,
    );
}

// Test: CBR wins (no results during capped trials).
// Set internalQueryPlanEvaluationWorks to 1, so each plan gets 0 works during the capped trial.
// The capped trial produces no results and does not exit early, triggering CBR.
{
    const originalKnobValue = assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 1}),
    ).was;

    coll.getPlanCache().clear();

    const cbrBefore = getCBRMetrics();
    const mpBefore = getMultiPlannerMetrics();
    const planningBefore = getPlanningMetrics();

    assert.commandWorked(coll.find(nonProductiveFilterWithMultipleSolutions).explain());

    const cbrAfter = getCBRMetrics();
    const mpAfter = getMultiPlannerMetrics();
    const planningAfter = getPlanningMetrics();

    // CBR should have been invoked and chosen the winning plan.
    assert.eq(
        cbrAfter.count,
        cbrBefore.count + 1,
        `cbr.count should increase by 1 when CBR wins in automaticCE. Previous value: ${cbrBefore.count} Current value: ${cbrAfter.count}`,
    );
    assert.eq(
        cbrAfter.choseWinningPlan,
        cbrBefore.choseWinningPlan + 1,
        `cbr.choseWinningPlan should increase by 1 when CBR wins in automaticCE. Previous value: ${cbrBefore.choseWinningPlan} Current value: ${cbrAfter.choseWinningPlan}`,
    );

    // Multiplanner trial metrics should still be incremented (the initial capped trials did run).
    assert.eq(
        mpAfter.classicCount,
        mpBefore.classicCount + 1,
        `multiPlanner.classicCount should increase by 1 for the capped trial. Previous value: ${mpBefore.classicCount} Current value: ${mpAfter.classicCount}`,
    );

    // Planning should have been invoked once for the explain (capped trials then CBR).
    assert.eq(
        planningAfter.invocationCount,
        planningBefore.invocationCount + 1,
        `planning.invocationCount should increase by 1 when CBR wins in automaticCE. Previous value: ${planningBefore.invocationCount} Current value: ${planningAfter.invocationCount}`,
    );

    // But multiPlanner.choseWinningPlan should NOT increase since CBR chose the winner.
    assert.eq(
        mpAfter.choseWinningPlan,
        mpBefore.choseWinningPlan,
        "multiPlanner.choseWinningPlan should not change when CBR wins",
    );

    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: originalKnobValue}));
}

MongoRunner.stopMongod(conn);

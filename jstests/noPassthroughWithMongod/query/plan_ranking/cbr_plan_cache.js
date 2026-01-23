/**
 * Establish the multiplanning replanning behavior that we would like to preserve.
 * TODO SERVER-116987: Verify that CBR is able to choose a plan that gets cached.
 * TODO SERVER-116351: Verify that CBR is able to be invoked during replanning.
 */

import {
    getPlanCacheShapeHashFromObject,
    getCachedPlanForQuery,
    assertPlanHasIxScanStage,
} from "jstests/libs/query/analyze_plan.js";

import {checkSbeFullFeatureFlagEnabled, checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

if (checkSbeFullFeatureFlagEnabled(db)) {
    jsTest.log.info("Skipping test because the SBE plan cache is enabled");
    quit();
}

// SBE plans can get cached in the classic plan cache, and some metrics are different for SBE plans
// in the classic cache vs classic plans in the classic cache.
const isSbeEnabled = checkSbeFullyEnabled(db);

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const docs = [];
const kNumDocs = 15000;

for (let i = 0; i < kNumDocs; i++) {
    docs.push({a: i, b: i});
}
docs.push({a: 7000 + 1, b: 7000 + 1, c: 1});
docs.push({a: 8000 + 1, b: 8000 + 1, c: 1});
coll.insertMany(docs);

// Enumerate 2 possible plans:
//  1) IXSCAN over 'a'
//  2) IXSCAN with 'b'
coll.createIndexes([{a: 1}, {b: 1}]);

// The predicate on 'b' is more selective.
const bIndexQuery = {a: {$gte: 1}, b: {$gte: 14500}, c: 1};
// The plans are actually tied for this query, but we deterministically choose the one with the index scan on 'a'.
const aIndexQuery = {a: {$gte: 1}, b: {$gte: 2}, c: 1};

function runInitialCacheTest() {
    coll.getPlanCache().clear();

    // aIndexQuery will hit the CBR fallback mechanism.
    let _ = coll.find(aIndexQuery).toArray();

    // The plan cache should now hold an inactive entry.
    let entry = getCachedPlanForQuery(db, coll, aIndexQuery);
    let planCacheShapeHash = getPlanCacheShapeHashFromObject(entry);
    assert.eq(entry.isActive, false);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "a_1", planCacheShapeHash);
    assert.eq(entry.creationExecStats.length, 2); // One for each candidate plan.
    assert.eq(entry.candidatePlanScores.length, 2); // One for each candidate plan.

    // TODO SERVER-116987: Add a log line that shows when CBR chose a plan that's cached.
    // And then check that log here when CBR is enabled. Note that it would be more ideal to check $planCacheStats here,
    // but the output of that does not differentiate between a CBR-chosen plan and a multiplanner-chosen plan.

    // Running the query again activates the cache entry.
    _ = coll.find(aIndexQuery).toArray();
    entry = getCachedPlanForQuery(db, coll, aIndexQuery);
    assert.eq(entry.isActive, true);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "a_1", planCacheShapeHash);
    assert.eq(entry.creationExecStats.length, 2); // One for each candidate plan.
    assert.eq(entry.candidatePlanScores.length, 2); // One for each candidate plan.

    // TODO SERVER-116987: Add a log line that shows when CBR chose a plan that's cached.
    // And then check that log here when CBR is enabled. Note that it would be more ideal to check $planCacheStats here,
    // but the output of that does not differentiate between a CBR-chosen plan and a multiplanner-chosen plan.
}

function runReplanningTest() {
    coll.getPlanCache().clear();

    let _ = coll.find(bIndexQuery).toArray();
    let currWorks = isSbeEnabled ? 1000 : 501;

    // The plan cache should now hold an inactive entry.
    let entry = getCachedPlanForQuery(db, coll, bIndexQuery);
    let planCacheShapeHash = getPlanCacheShapeHashFromObject(entry);
    let entryWorks = entry.works;
    assert.eq(entry.isActive, false);
    assert.eq(entryWorks, currWorks);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "b_1", planCacheShapeHash);

    // Running the query again activates the cache entry.
    _ = coll.find(bIndexQuery).toArray();
    entry = getCachedPlanForQuery(db, coll, bIndexQuery);
    assert.eq(entry.isActive, true);
    assert.eq(entry.works, entryWorks);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "b_1", planCacheShapeHash);
    assert.eq(entry.creationExecStats.length, 2); // One for each candidate plan.
    assert.eq(entry.candidatePlanScores.length, 2); // One for each candidate plan.

    // This query will trigger replanning since the number of works is vastly higher than the cached plan.
    // Because of this, the new plan will not be active at first.
    _ = coll.find(aIndexQuery).toArray();
    entry = getCachedPlanForQuery(db, coll, aIndexQuery);
    assert.eq(entry.isActive, false);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "a_1", planCacheShapeHash);
    assert.eq(entry.creationExecStats.length, 2); // One for each candidate plan.
    assert.eq(entry.candidatePlanScores.length, 2); // One for each candidate plan.

    // TODO SERVER-116351: Add a log line that shows when CBR chose a new plan via replanning.
    // And then check that log here when CBR is enabled. Note that it would be more ideal to check $planCacheStats here,
    // but the output of that does not differentiate between a CBR-chosen plan and a multiplanner-chosen plan.

    const growthCoefficient = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryCacheWorksGrowthCoefficient: 1}),
    ).internalQueryCacheWorksGrowthCoefficient;

    // Activate the plan. For this example, we need to run the query 5 times in order for the inactive cache
    // entry's number of works to grow larger (via the 'growthCoefficient') than the winning
    // plan's number of works, at which point the plan will be active.
    for (let i = 0; i < 5; i++) {
        // Cache entry should be inactive throughout this loop.
        assert(!entry.isActive);

        // When the number of works of the plan in the cache is less than the number of works taken to choose
        // the winning plan for this run of the query, the number of works in the cache entry grows by this coefficient.
        currWorks *= growthCoefficient;
        assert.eq(entry.works, currWorks);

        _ = coll.find(aIndexQuery).toArray();
        entry = getCachedPlanForQuery(db, coll, aIndexQuery);
    }

    // The cache entry is now active, with a number of works that is accurate to the aIndexQuery's number of works.
    // The 10000 (when SBE is not enabled) comes from the limit of the number of works the multiplanner can do
    // before it chooses a winning plan. In this case, since there are only 2 documents that match the query,
    // by the time we get to 10000 works, we will not have filled a batch. When SBE is enabled we record the
    // number of keys examined (10000) + the number of docs examined (10000) at the time we stop multiplanning
    // for the plan cache entry.
    assert(entry.isActive);
    assert.eq(entry.works, isSbeEnabled ? 20000 : 10000);
}

// TODO SERVER-116353: Add additional tests.

const prevPlanRankerMode = assert.commandWorked(
    db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}),
).was;
try {
    // TODO SERVER-116987: Run test under the two fallback strategies.
    runInitialCacheTest();

    // TODO SERVER-116351: Run test under the two fallback strategies.
    runReplanningTest();

    // TODO SERVER-116989: Run tests under the non-release CBR configurations (e.g. sampling).
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: prevPlanRankerMode}));
}

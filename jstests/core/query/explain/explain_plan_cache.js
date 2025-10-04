/**
 * Tests that the "isCached" field on all explain variants work properly for different types of
 * queries, including collscans, find, aggregate, and queries that are cached and then become
 * suboptimal due to a change in data.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   # Exercises a bug in distinct scan hashing that was fixed in 8.2.
 *   requires_fcv_82,
 *   # Does not support multiplanning, because it caches more plans
 *   does_not_support_multiplanning_single_solutions,
 *   # The test examines the SBE plan cache, which initial sync may change the contents of.
 *   examines_sbe_cache,
 *   # The test runs commands that are not allowed with security token: planCacheClear.
 *   not_allowed_with_signed_security_token
 * ]
 */
import {
    getAggPlanStage,
    getQueryPlanner,
    getRejectedPlan,
    getRejectedPlans,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled, checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const shouldGenerateSbePlan = checkSbeFullyEnabled(db);
const isUsingSbePlanCache = checkSbeFullFeatureFlagEnabled(db);
const coll = db.explain_plan_cache;

// Assert the winning plan is cached and rejected are not.
function assertWinningPlanCacheStatus(explain, status) {
    const winningPlan = shouldGenerateSbePlan
        ? getQueryPlanner(explain).winningPlan
        : getWinningPlanFromExplain(explain);
    assert.eq(winningPlan.isCached, status, explain);
    for (let rejectedPlan of getRejectedPlans(explain)) {
        rejectedPlan = getRejectedPlan(rejectedPlan);
        assert(!rejectedPlan.isCached, explain);
    }
}

// Assert the winning plan is not cached and a rejected plan using the given name is cached.
function assertRejectedPlanCached(explain, indexName) {
    const winningPlan = shouldGenerateSbePlan
        ? getQueryPlanner(explain).winningPlan
        : getWinningPlanFromExplain(explain);
    assert(!winningPlan.isCached, explain);
    for (const rejectedPlan of getRejectedPlans(explain)) {
        const inputStage = getRejectedPlan(rejectedPlan).inputStage;
        if (inputStage.stage === "IXSCAN" && inputStage.indexName === indexName) {
            assert(rejectedPlan.isCached, explain);
        } else {
            assert(!rejectedPlan.isCached, explain);
        }
    }
}

function getAggPlannerExplain(explainMode, pipeline) {
    const explain = coll.explain(explainMode).aggregate(pipeline);
    return getAggPlanStage(explain, "$cursor")["$cursor"];
}

// Assert that collscans in explain report isCached correctly.
function collScanTest(explainMode) {
    coll.drop();
    coll.insert({a: 1, b: 1});
    assertWinningPlanCacheStatus(coll.find({a: 1, b: 1}).explain(explainMode), false);

    // Run the query so it gets cached (for SBE).
    for (let i = 0; i < 5; i++) {
        coll.find({a: 1, b: 1}).toArray();
    }
    assertWinningPlanCacheStatus(coll.find({a: 2, b: 2}).explain(explainMode), isUsingSbePlanCache);
}

// Tests basic find and aggregations that share the same cache entries report isCached correctly.
function predicateTest(explainMode) {
    coll.drop();
    coll.insert({a: 1});

    // Create indexes so we have plan alternatives and need to cache.
    coll.createIndex({a: 1});
    coll.createIndex({b: 1});
    coll.createIndex({a: -1, b: 1});
    coll.createIndex({b: -1, a: 1});

    // Nothing should be cached at first.
    assertWinningPlanCacheStatus(coll.find({a: {$eq: 1}}).explain(explainMode), false);
    assertWinningPlanCacheStatus(getAggPlannerExplain(explainMode, [{$match: {a: 1}}, {$unwind: "$d"}]), false);

    // Run the query to get it cached.
    for (let i = 0; i < 5; i++) {
        coll.find({a: {$eq: 1}}).toArray();
    }

    assert.eq(coll.getPlanCache().list().length, 1);

    // For both find and agg we should have the winning plan cached. Use different values in the
    // predicates to show the hash is indifferent to the value.
    assertWinningPlanCacheStatus(coll.find({a: {$eq: 2}}).explain(explainMode), true);
    assertWinningPlanCacheStatus(getAggPlannerExplain(explainMode, [{$match: {a: 2}}, {$unwind: "$d"}]), true);

    // Test with rooted OR.
    coll.getPlanCache().clear();
    for (let i = 0; i < 5; i++) {
        coll.find({$or: [{a: {$eq: 1}}, {b: {$eq: 1}}]}).toArray();
    }
    // The query will use sub-planning so don't expect the top-level query to hit the cache when SBE
    // isn't enabled. However when SBE is fully enabled, we cache the full plan.
    assertWinningPlanCacheStatus(
        coll.find({$or: [{a: {$eq: 4}}, {b: {$eq: 4}}]}).explain(explainMode),
        isUsingSbePlanCache,
    );

    // Test with a contained OR. The query will be planned as a whole so we do expect it to hit the
    // cache.
    coll.getPlanCache().clear();
    for (let i = 0; i < 5; i++) {
        coll.find({
            $and: [{$or: [{a: 10}, {b: {$gt: 99}}]}, {$or: [{a: {$in: [5, 1]}}, {b: {$in: [7, 99]}}]}],
        }).toArray();
    }
    assert.eq(coll.getPlanCache().list().length, 1);
    assertWinningPlanCacheStatus(
        coll
            .find({
                $and: [{$or: [{a: 2}, {b: {$gt: 100}}]}, {$or: [{a: {$in: [6, 2]}}, {b: {$in: [7, 100]}}]}],
            })
            .explain(explainMode),
        true,
    );
}

function sortOnlyTest(explainMode) {
    coll.drop();
    assert.commandWorked(coll.insert({_id: 1, a: NumberInt(9)}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({a: -1}));

    // Nothing should be cached at first.
    assert.eq(coll.getPlanCache().list().length, 0);
    assertWinningPlanCacheStatus(coll.find().sort({a: 1}).explain(explainMode), false);

    // Run the query to get it cached.
    for (let i = 0; i < 5; i++) {
        coll.find().sort({a: 1}).toArray();
    }

    assert.eq(coll.getPlanCache().list().length, 1);

    // Only the winning plan should have 'isCached' set to true.
    assertWinningPlanCacheStatus(coll.find().sort({a: 1}).explain(explainMode), true);
}

/*
 * Assert that when a plan is cached, and the data changes so that the cached plan is no longer
 * optimal, we report isCached=true for the correct rejected plan.
 * In previous tests, we've run a query, and then run explain with the same query but different
 * constant values to make sure the hash is indifferent to constants. For the test we cannot do that
 * because the result that the query planner picks depends on the data and the particular constants
 * we use.
 */
function dataChangeTest(explainMode) {
    coll.drop();
    coll.insert({a: 1, b: 1});
    coll.createIndex({a: 1});
    coll.createIndex({b: 1});

    let arr = [];
    for (let i = 0; i < 1000; i++) {
        arr.push({a: i, b: 1});
    }
    coll.insert(arr);

    assertWinningPlanCacheStatus(coll.find({a: 1, b: 1}).explain(explainMode), false);

    // Run the query to get it cached.
    for (let i = 0; i < 5; i++) {
        coll.find({a: 1, b: 1}).toArray();
    }
    assert.eq(coll.getPlanCache().list().length, 1);
    // The plan using the "a" index will win, we assert that it's cached.
    assertWinningPlanCacheStatus(coll.find({a: 1, b: 1}).explain(explainMode), true);

    // Change the data so that the "b" index plan will be better.
    coll.deleteMany({b: 1});
    arr = [];
    for (let i = 0; i < 1000; i++) {
        arr.push({a: 1, b: i});
    }
    coll.insert(arr);
    // The "b" indexed plan should win, but the "a" plan is still cached. So we'll see a
    // rejected plan that has isCached=true.
    assertRejectedPlanCached(coll.find({a: 1, b: 1}).explain(explainMode), "a_1");

    // Now rerun the plan a few times, and we should replan to get the "b" plan cached.
    for (let i = 0; i < 5; i++) {
        coll.find({a: 1, b: 1}).toArray();
    }
    assertWinningPlanCacheStatus(coll.find({a: 1, b: 1}).explain(explainMode), true);
}

function distinctScanTest(explainMode) {
    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));

    const pipeline = [{$sort: {a: 1}}, {$group: {_id: "$a"}}];

    // Nothing should be cached at first.
    assertWinningPlanCacheStatus(coll.explain(explainMode).aggregate(pipeline), false);

    // Run the query to get it cached.
    for (let i = 0; i < 5; i++) {
        coll.aggregate(pipeline).toArray();
    }

    // Only the winning plan should have 'isCached' set to true.
    assertWinningPlanCacheStatus(coll.explain(explainMode).aggregate(pipeline), true);
}

for (const explainMode of ["queryPlanner", "executionStats", "allPlansExecution"]) {
    collScanTest(explainMode);
    predicateTest(explainMode);
    dataChangeTest(explainMode);
    sortOnlyTest(explainMode);
    distinctScanTest(explainMode);
}

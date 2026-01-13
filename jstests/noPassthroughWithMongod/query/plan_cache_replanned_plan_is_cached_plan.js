/**
 * This test emulates a situation when a query for which there is an existing plan in the plan cache
 * is replanned and produces the same plan during replanning. This is done by creating a collection
 * with indices A and B, and a varying set of documents. A will always be the ideal index, but the
 * first query will be a low-works query that activates a low-works plan in the plan cache. The
 * other query changes a predicate value to increase the works that the cached plan will take,
 * triggering a replan, but the replanned plan will still be the same plan.
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    getPlanCacheShapeHashFromObject,
    getCachedPlanForQuery,
    assertPlanHasIxScanStage,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

const isSbePlanCacheEnabled = checkSbeFullFeatureFlagEnabled(db);

let coll = assertDropAndRecreateCollection(db, "plan_cache_replanning");

function getReplannedMetric() {
    const planCacheType = isSbePlanCacheEnabled ? "sbe" : "classic";
    return assert.commandWorked(db.serverStatus()).metrics.query.planCache[planCacheType].replanned;
}

function getReplannedPlanIsCachedPlanMetric() {
    const planCacheType = isSbePlanCacheEnabled ? "sbe" : "classic";
    return assert.commandWorked(db.serverStatus()).metrics.query.planCache[planCacheType].replanned_plan_is_cached_plan;
}

// Carefully construct a collection so that some queries will have fewer works with an {a: 1} index
// other with greater works.
assert.commandWorked(
    coll.insertMany(
        Array.from({length: 1000}, (_, index) => {
            return {a: Math.min(10, index), b: 1};
        }),
    ),
);

// This query will be quick because there is only one matching document.
const cheapQuery = {
    a: 1,
    b: 1,
};
// This query will be expensive because there are 990 matching documents.
const expensiveQuery = {
    a: 10,
    b: 1,
};

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Run the first query to generate the cached plan.
assert.eq(1, coll.find(cheapQuery).itcount());

// The plan cache should now hold an inactive entry. Both queries are the same shape, so cheapQuery
// should produce plans that can be associated with expensiveQuery as well.
let entry = getCachedPlanForQuery(db, coll, expensiveQuery);
let planCacheShapeHash = getPlanCacheShapeHashFromObject(entry);
assert.eq(getPlanCacheShapeHashFromObject(getCachedPlanForQuery(db, coll, cheapQuery)), planCacheShapeHash);

let entryWorks = entry.works;
assert.eq(entry.isActive, false);
assertPlanHasIxScanStage(isSbePlanCacheEnabled, entry, "a_1", planCacheShapeHash);

// Re-run the query. The inactive cache entry should be promoted to an active entry.
assert.eq(1, coll.find(cheapQuery).itcount());
entry = getCachedPlanForQuery(db, coll, expensiveQuery);
assert.eq(entry.isActive, true);
assert.eq(entry.works, entryWorks);
assertPlanHasIxScanStage(isSbePlanCacheEnabled, entry, "a_1", planCacheShapeHash);

// Now run the expensiveQuery and expect a replan with the same plan.
{
    const replannedMetric = getReplannedMetric();
    const replannedPlanIsCachedPlanMetric = getReplannedPlanIsCachedPlanMetric();
    assert.eq(990, coll.find(expensiveQuery).itcount());
    entry = getCachedPlanForQuery(db, coll, expensiveQuery);
    assert.eq(entry.isActive, false);
    assertPlanHasIxScanStage(isSbePlanCacheEnabled, entry, "a_1", planCacheShapeHash);
    assert.eq(replannedMetric + 1, getReplannedMetric());
    assert.eq(replannedPlanIsCachedPlanMetric + 1, getReplannedPlanIsCachedPlanMetric());
}

// Nothing should be new with cheapQuery, though.
{
    const replannedMetric = getReplannedMetric();
    const replannedPlanIsCachedPlanMetric = getReplannedPlanIsCachedPlanMetric();
    assert.eq(1, coll.find(cheapQuery).itcount());
    entry = getCachedPlanForQuery(db, coll, cheapQuery);
    assert.eq(entry.isActive, true);
    assertPlanHasIxScanStage(isSbePlanCacheEnabled, entry, "a_1", planCacheShapeHash);
    assert.eq(replannedMetric, getReplannedMetric());
    assert.eq(replannedPlanIsCachedPlanMetric, getReplannedPlanIsCachedPlanMetric());
}

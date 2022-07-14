/**
 * Test that cached plans which use wildcard indexes work.
 *
 * @tags: [
 *   # This test attempts to perform queries and introspect the server's plan cache entries using
 *   # the $planCacheStats aggregation source. Both operations must be routed to the primary, and
 *   # the latter only supports 'local' readConcern.
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged,
 *   does_not_support_stepdowns,
 *   # If the balancer is on and chunks are moved, the plan cache can have entries with isActive:
 *   # false when the test assumes they are true because the query has already been run many times.
 *   assumes_balancer_off,
 *   inspects_whether_plan_cache_entry_is_active,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');              // For getPlanStage().
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load('jstests/libs/fixture_helpers.js');  // For getPrimaryForNodeHostingDatabase and isMongos.
load("jstests/libs/sbe_util.js");         // For checkSBEEnabled.

const coll = db.wildcard_cached_plans;
coll.drop();

assert.commandWorked(coll.createIndex({"b.$**": 1}));
assert.commandWorked(coll.createIndex({"a": 1}));

const isSbeEnabled = checkSBEEnabled(db, ["featureFlagSbeFull"]);

// In order for the plan cache to be used, there must be more than one plan available. Insert
// data into the collection such that the b.$** index will be far more selective than the index
// on 'a' for the query {a: 1, b: 1}.
for (let i = 0; i < 1000; i++) {
    assert.commandWorked(coll.insert({a: 1}));
}
assert.commandWorked(coll.insert({a: 1, b: 1}));

function getCacheEntryForQuery(query) {
    const match = {
        planCacheKey: getPlanCacheKeyFromShape({query: query, collection: coll, db: db})
    };
    const aggRes = FixtureHelpers.getPrimaryForNodeHostingDatabase(db)
                       .getCollection(coll.getFullName())
                       .aggregate([{$planCacheStats: {}}, {$match: match}])
                       .toArray();
    assert.lte(aggRes.length, 1);
    if (aggRes.length > 0) {
        return aggRes[0];
    }
    return null;
}

const query = {
    a: 1,
    b: 1
};

// The plan cache should be empty.
assert.eq(getCacheEntryForQuery(query), null);

// Run the query twice, once to create the cache entry, and again to make the cache entry
// active.
for (let i = 0; i < 2; i++) {
    assert.eq(coll.find(query).itcount(), 1);
}

// The plan cache should no longer be empty. Check that the chosen plan uses the b.$** index.
let cacheEntry = getCacheEntryForQuery(query);
assert.neq(cacheEntry, null);
assert.eq(cacheEntry.isActive, true);
if (!isSbeEnabled) {
    // Should be at least two plans: one using the {a: 1} index and the other using the b.$** index.
    assert.gte(cacheEntry.creationExecStats.length, 2, tojson(cacheEntry.plans));

    const ixscan = function() {
        const execStats = cacheEntry.creationExecStats;
        if (!execStats)
            return null;
        const elem = execStats[0];
        if (!elem)
            return null;
        if (!elem.executionStages)
            return null;
        return getPlanStage(elem.executionStages, "IXSCAN");
    }();
    const expectedKeyPattern = {"$_path": 1, "b": 1};
    assert.neq(null, ixscan, cacheEntry);
    assert(bsonWoCompare(ixscan.keyPattern, expectedKeyPattern) === 0, ixscan);
} else {
    assert(cacheEntry.hasOwnProperty("cachedPlan"), cacheEntry);
    assert(cacheEntry.cachedPlan.hasOwnProperty("stages"), cacheEntry);
    const sbePlan = cacheEntry.cachedPlan.stages;
    // The SBE plan string should contain the name of the b.$** index.
    assert(sbePlan.includes("b.$**_1"), cacheEntry);
}

// Run the query again. This time it should use the cached plan. We should get the same result
// as earlier.
assert.eq(coll.find(query).itcount(), 1);

// Now run a query where b is null. This should have a different shape key from the previous
// query since $** indexes are sparse.
const queryWithBNull = {
    a: 1,
    b: null
};
for (let i = 0; i < 2; i++) {
    assert.eq(coll.find({a: 1, b: null}).itcount(), 1000);
}
assert.neq(getPlanCacheKeyFromShape({query: queryWithBNull, collection: coll, db: db}),
           getPlanCacheKeyFromShape({query: query, collection: coll, db: db}));

// There should only have been one solution for the above query, so it would get cached only by the
// SBE plan cache.
cacheEntry = getCacheEntryForQuery({a: 1, b: null});
if (isSbeEnabled) {
    assert.neq(cacheEntry, null);
    assert.eq(cacheEntry.isActive, true, cacheEntry);
    assert.eq(cacheEntry.isPinned, true, cacheEntry);
} else {
    assert.eq(cacheEntry, null);
}

// Check that indexability discriminators work with collations.
(function() {
// Create wildcard index with a collation.
assertDropAndRecreateCollection(db, coll.getName(), {collation: {locale: "en_US", strength: 1}});
assert.commandWorked(coll.createIndex({"b.$**": 1}));

// Run a query which uses a different collation from that of the index, but does not use
// string bounds.
const queryWithoutStringExplain =
    coll.explain().find({a: 5, b: 5}).collation({locale: "fr"}).finish();
let ixScans = getPlanStages(getWinningPlan(queryWithoutStringExplain.queryPlanner), "IXSCAN");
assert.eq(ixScans.length, FixtureHelpers.numberOfShardsForCollection(coll));
assert.eq(ixScans[0].keyPattern, {$_path: 1, b: 1});

// Run a query which uses a different collation from that of the index and does have string
// bounds.
const queryWithStringExplain =
    coll.explain().find({a: 5, b: "a string"}).collation({locale: "fr"}).finish();
ixScans = getPlanStages(getWinningPlan(queryWithStringExplain.queryPlanner), "IXSCAN");
assert.eq(ixScans.length, 0);

// Check that the shapes are different since the query which matches on a string will not
// be eligible to use the b.$** index (since the index has a different collation).
assert.neq(getPlanCacheKeyFromExplain(queryWithoutStringExplain, db),
           getPlanCacheKeyFromExplain(queryWithStringExplain, db));
})();

// Check that indexability discriminators work with partial wildcard indexes.
(function() {
assertDropAndRecreateCollection(db, coll.getName());
assert.commandWorked(coll.createIndex({"$**": 1}, {partialFilterExpression: {a: {$lte: 5}}}));

// Run a query for a value included by the partial filter expression.
const queryIndexedExplain = coll.find({a: 4}).explain();
let ixScans = getPlanStages(getWinningPlan(queryIndexedExplain.queryPlanner), "IXSCAN");
assert.eq(ixScans.length, FixtureHelpers.numberOfShardsForCollection(coll));
assert.eq(ixScans[0].keyPattern, {$_path: 1, a: 1});

// Run a query which tries to get a value not included by the partial filter expression.
const queryUnindexedExplain = coll.find({a: 100}).explain();
ixScans = getPlanStages(getWinningPlan(queryUnindexedExplain.queryPlanner), "IXSCAN");
assert.eq(ixScans.length, 0);

// Check that the shapes are different since the query which searches for a value not
// included by the partial filter expression won't be eligible to use the $** index.
assert.neq(getPlanCacheKeyFromExplain(queryIndexedExplain, db),
           getPlanCacheKeyFromExplain(queryUnindexedExplain, db));
})();
})();

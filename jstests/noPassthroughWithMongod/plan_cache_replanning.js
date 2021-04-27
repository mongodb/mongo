/**
 * This test will attempt to create a scenario where the plan cache entry for a given query shape
 * oscillates. It achieves this by creating two indexes, A and B, on a collection, and interleaving
 * queries which are "ideal" for index A with queries that are "ideal" for index B.
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');              // For getPlanStage().
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

const coll = assertDropAndRecreateCollection(db, "plan_cache_replanning");

function getPlansForCacheEntry(match) {
    const matchingCacheEntries = coll.getPlanCache().list([{$match: match}]);
    assert.eq(matchingCacheEntries.length, 1, coll.getPlanCache().list());
    return matchingCacheEntries[0];
}

function planHasIxScanStageForKey(planStats, keyPattern) {
    const stage = getPlanStage(planStats, "IXSCAN");
    if (stage === null) {
        return false;
    }

    return bsonWoCompare(keyPattern, stage.keyPattern) == 0;
}

// Carefully construct a collection so that some queries will do well with an {a: 1} index
// and others with a {b: 1} index.
for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: 1, b: i}));
}

for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: i, b: 2}));
}

// This query will be quick with {a: 1} index, and far slower {b: 1} index. With the {a: 1}
// index, the server should only need to examine one document. Using {b: 1}, it will have to
// scan through each document which has 2 as the value of the 'b' field.
const aIndexQuery = {
    a: 1099,
    b: 2
};
// Opposite of 'aIndexQuery'. Should be quick if the {b: 1} index is used, and slower if the
// {a: 1} index is used.
const bIndexQuery = {
    a: 1,
    b: 1099
};

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Run a query where the {b: 1} index will easily win.
assert.eq(1, coll.find(bIndexQuery).itcount());

// The plan cache should now hold an inactive entry.
let entry = getPlansForCacheEntry({"createdFromQuery.query": bIndexQuery});
let entryWorks = entry.works;
assert.eq(entry.isActive, false);
assert.eq(planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), {b: 1}), true, entry);

// Get the hash of the query shape so that we keep looking up entries associated with the same shape
// going forward.
const queryHash = entry.queryHash;

// Re-run the query. The inactive cache entry should be promoted to an active entry.
assert.eq(1, coll.find(bIndexQuery).itcount());
entry = getPlansForCacheEntry({queryHash: queryHash});
assert.eq(entry.isActive, true);
assert.eq(entry.works, entryWorks);
assert.eq(planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), {b: 1}), true, entry);

// Now we will attempt to oscillate the cache entry by interleaving queries which should use
// the {a:1} and {b:1} index. When the plan using the {b: 1} index is in the cache, running a
// query which should use the {a: 1} index will perform very poorly, and trigger
// replanning (and vice versa).

// The {b: 1} plan is currently in the cache. Run the query which should use the {a: 1}
// index. The current cache entry will be deactivated, and then the cache entry for the {a: 1}
// will overwrite it (as active).
assert.eq(1, coll.find(aIndexQuery).itcount());
entry = getPlansForCacheEntry({queryHash: queryHash});
assert.eq(entry.isActive, true);
assert.eq(planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), {a: 1}), true, entry);

// Run the query which should use the {b: 1} index.
assert.eq(1, coll.find(bIndexQuery).itcount());
entry = getPlansForCacheEntry({queryHash: queryHash});
assert.eq(entry.isActive, true);
assert.eq(planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), {b: 1}), true, entry);

// The {b: 1} plan is again in the cache. Run the query which should use the {a: 1}
// index.
assert.eq(1, coll.find(aIndexQuery).itcount());
entry = getPlansForCacheEntry({queryHash: queryHash});
assert.eq(entry.isActive, true);
assert.eq(planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), {a: 1}), true, entry);

// The {a: 1} plan is back in the cache. Run the query which would perform better on the plan
// using the {b: 1} index, and ensure that plan gets written to the cache.
assert.eq(1, coll.find(bIndexQuery).itcount());
entry = getPlansForCacheEntry({queryHash: queryHash});
entryWorks = entry.works;
assert.eq(entry.isActive, true);
assert.eq(planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), {b: 1}), true, entry);

// Now run a plan that will perform poorly with both indices (it will be required to scan 500
// documents). This will result in replanning (and the cache entry being deactivated). However,
// the new plan will have a very high works value, and will not replace the existing cache
// entry. It will only bump the existing cache entry's works value.
for (let i = 0; i < 500; i++) {
    assert.commandWorked(coll.insert({a: 3, b: 3}));
}
assert.eq(500, coll.find({a: 3, b: 3}).itcount());

// The cache entry should have been deactivated.
entry = getPlansForCacheEntry({queryHash: queryHash});
assert.eq(entry.isActive, false);
assert.eq(planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), {b: 1}), true, entry);

// The works value should have doubled.
assert.eq(entry.works, entryWorks * 2);
})();

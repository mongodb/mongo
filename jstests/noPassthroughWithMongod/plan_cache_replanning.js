/**
 * This test will attempt to create a scenario where the plan cache entry for a given query shape
 * oscillates. It achieves this by creating two indexes, A and B, on a collection, and interleaving
 * queries which are "ideal" for index A with queries that are "ideal" for index B.
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');              // For getPlanStage().
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

let coll = assertDropAndRecreateCollection(db, "plan_cache_replanning");

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

// Drop and recreate the collection. Now we test that the query system does not replan in cases
// where the plan is performing only slightly less efficiently than the cached plan.
coll = assertDropAndRecreateCollection(db, "plan_cache_replanning");

{
    assert.commandWorked(coll.createIndex({selectiveKey: 1, tiebreak: 1}));
    assert.commandWorked(coll.createIndex({notSelectiveKey: 1, tiebreak: 1}));

    // These are the possible values used for the 'tiebreak' field. The 'tiebreak' field is included
    // to guarantee that certain documents are inspected before others to ensure that the plan
    // may see documents which don't match the filter and "waste" work reading these documents.
    const kTieBreakLow = 0;
    const kTieBreakHigh = 1;

    // Special value of 'selectiveKey' for which the plan using {selectiveKey:1, tiebreak:1} is
    // *slightly* less efficient, but still far better than the plan using {nonSelectiveKey:1,
    // tiebreak:1}.
    const kSpecialSelectiveKey = 2;

    // The query using 'filterOnSelectiveKey' should require slightly fewer reads than
    // 'filterOnSelectiveKeySpecialValue'. We will check that running
    // 'filterOnSelectiveKeySpecialValue' when there is a cache entry generated from
    // 'filterOnSelectiveKey' does *not* cause replanning.
    const filterOnSelectiveKey = {notSelectiveKey: {$lt: 50}, selectiveKey: 3};
    const filterOnSelectiveKeySpecialValue = {
        notSelectiveKey: {$lt: 50},
        selectiveKey: kSpecialSelectiveKey
    };

    // Insert 110 documents for each value of selectiveKey from 1-10. We use the number 110 docs
    // because it is greater than the because it is greater than the predefined doc limit of 101
    // for cached planning and multi-planning.
    for (let i = 0; i < 10; ++i) {
        for (let j = 0; j < 110; ++j) {
            assert.commandWorked(
                coll.insert({notSelectiveKey: 10, selectiveKey: i, tiebreak: kTieBreakHigh}));
        }
    }

    // Now add one extra document so the plan requires a few more reads/works when the value of
    // 'selectiveKey' is 'kSpecialSelectiveKey'. We use a low value of 'tiebreak' to ensure that
    // this special, non-matching document is inspected before the documents which do match the
    // filter.
    assert.commandWorked(coll.insert(
        {notSelectiveKey: 55, selectiveKey: kSpecialSelectiveKey, tiebreak: kTieBreakLow}));

    // Now we run a query using the special value of 'selectiveKey' until the plan gets cached. We
    // run it twice to make the cache entry active.
    for (let i = 0; i < 2; ++i) {
        assert.eq(110, coll.find(filterOnSelectiveKeySpecialValue).itcount());
    }

    // Now look at the cache entry and store the values for works, keysExamined.
    entry = getPlansForCacheEntry({"createdFromQuery.query": filterOnSelectiveKeySpecialValue});
    const specialValueCacheEntryWorks = entry.works;
    const specialValueCacheEntryKeysExamined = entry.creationExecStats[0].totalKeysExamined;

    assert.eq(entry.isActive, true, entry);
    assert.eq(
        planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), {selectiveKey: 1, tiebreak: 1}),
        true,
        entry);

    // Clear the plan cache for the collection.
    coll.getPlanCache().clear();

    // Now run the query on the non-special value of 'selectiveKey' until it gets cached.
    for (let i = 0; i < 2; ++i) {
        assert.eq(110, coll.find(filterOnSelectiveKey).itcount());
    }

    entry = getPlansForCacheEntry({"createdFromQuery.query": filterOnSelectiveKey});
    assert.eq(entry.isActive, true, entry);
    assert.eq(
        planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), {selectiveKey: 1, tiebreak: 1}),
        true,
        entry);

    // The new cache entry's plan should have used fewer works (and examined fewer keys) compared
    // to the old cache entry's, since the query on the special value is slightly less efficient.
    assert.lt(entry.works, specialValueCacheEntryWorks, entry);
    assert.lt(
        entry.creationExecStats[0].totalKeysExamined, specialValueCacheEntryKeysExamined, entry);

    // Now run the query on the "special" value again and check that replanning does not happen
    // even though the plan is slightly less efficient than the one in the cache.
    assert.eq(110, coll.find(filterOnSelectiveKeySpecialValue).itcount());

    // Check that the cache entry hasn't changed.
    const entryAfterRunningSpecialQuery =
        getPlansForCacheEntry({"createdFromQuery.query": filterOnSelectiveKey});
    assert.eq(entryAfterRunningSpecialQuery.isActive, true);
    assert.eq(planHasIxScanStageForKey(getCachedPlan(entryAfterRunningSpecialQuery.cachedPlan),
                                       {selectiveKey: 1, tiebreak: 1}),
              true,
              entry);

    assert.eq(entry.works, entryAfterRunningSpecialQuery.works, entryAfterRunningSpecialQuery);
    assert.eq(entryAfterRunningSpecialQuery.creationExecStats[0].totalKeysExamined,
              entry.creationExecStats[0].totalKeysExamined,
              entryAfterRunningSpecialQuery);
}
})();

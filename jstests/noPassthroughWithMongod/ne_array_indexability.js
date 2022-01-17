/**
 * Test that $ne: [] queries are cached correctly. See SERVER-39764.
 */
(function() {
load("jstests/libs/analyze_plan.js");  // For getPlanCacheKeyFromShape.
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.

const isSbePlanCacheEnabled = checkSBEEnabled(db, ["featureFlagSbePlanCache"]);
const coll = db.ne_array_indexability;
coll.drop();

coll.createIndex({"obj": 1});
coll.createIndex({"obj": 1, "abc": 1});

assert.commandWorked(coll.insert({obj: "hi there"}));

function runTest(queryToCache, queryToRunAfterCaching) {
    assert.eq(coll.find(queryToCache).itcount(), 1);
    assert.eq(coll.find(queryToCache).itcount(), 1);

    const keyHash = getPlanCacheKeyFromShape({query: queryToCache, collection: coll, db});
    const match = {planCacheKey: keyHash};
    const cacheEntries = coll.aggregate([{$planCacheStats: {}}, {$match: match}]).toArray();
    assert.eq(cacheEntries.length, 1);
    assert.eq(cacheEntries[0].isActive, true);

    assert.eq(coll.find(queryToRunAfterCaching).itcount(), 1);

    const explain = assert.commandWorked(coll.find(queryToRunAfterCaching).explain());
    // TODO SERVER-61314: Remove this check when "featureFlagSbePlanCache" is removed.
    // SBE plan cache key currently encodes the entire filter, so queryHash should be different too.
    if (!isSbePlanCacheEnabled) {
        // The query with the $ne: array should have the same queryHash, but a different
        // planCacheKey.
        assert.eq(explain.queryPlanner.queryHash, cacheEntries[0].queryHash);
    }
    assert.neq(explain.queryPlanner.planCacheKey, cacheEntries[0].planCacheKey);
}

runTest({'obj': {$ne: 'def'}}, {'obj': {$ne: [[1]]}});

// Clear the cache.
assert.commandWorked(coll.runCommand('planCacheClear'));

runTest({'obj': {$nin: ['abc', 'def']}}, {'obj': {$nin: [[1], 'abc']}});
})();

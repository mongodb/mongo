/**
 * Test that $ne: [] queries are cached correctly. See SERVER-39764.
 */
import {
    getPlanCacheKeyFromShape,
    getPlanCacheShapeHashFromObject
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

const isUsingSbePlanCache = checkSbeFullFeatureFlagEnabled(db);

const coll = db.ne_array_indexability;
coll.drop();

coll.createIndex({"obj": 1});
coll.createIndex({"obj": -1});

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

    // For the classic plan cache, the query with the $ne: array should have the same
    // 'planCacheShapeHash', but a different 'planCacheKey'. The SBE plan cache, on the other hand,
    // does not auto-parameterize $in or $eq involving a constant of type array, and therefore will
    // consider the two queries to have different shapes.
    if (isUsingSbePlanCache) {
        assert.neq(explain.queryPlanner.planCacheShapeHash, cacheEntries[0].planCacheShapeHash);
    } else {
        assert.eq(explain.queryPlanner.planCacheShapeHash, cacheEntries[0].planCacheShapeHash);
    }

    // For both the classic and SBE plan caches, the two queries must have different plan cache
    // keys.
    assert.neq(explain.queryPlanner.planCacheKey, cacheEntries[0].planCacheKey);
}

runTest({'obj': {$ne: 'def'}}, {'obj': {$ne: [[1]]}});

// Clear the cache.
assert.commandWorked(coll.runCommand('planCacheClear'));

runTest({'obj': {$nin: ['abc', 'def']}}, {'obj': {$nin: [[1], 'abc']}});

/**
 * Tests that auto-parameterized index scan plans are correctly stored in the SBE plan cache, and
 * that they can be correctly recovered from the cache with new parameter values.
 *
 * @tags: [
 *   not_allowed_with_signed_security_token,
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   # The SBE plan cache was enabled by default in 6.3.
 *   requires_fcv_63,
 *   # Plan cache state is node-local and will not get migrated alongside user data.
 *   tenant_migration_incompatible,
 *   assumes_balancer_off,
 *   # This test is specifically verifying the behavior of the SBE plan cache, which is only enabled
 *   # when SBE is enabled.
 *   featureFlagSbeFull,
 * ]
 */
import {getPlanCacheKeyFromExplain, getQueryHashFromExplain} from "jstests/libs/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

// Set up the collection with an index and a set of documents.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insertMany([{_id: 1, a: 1}, {_id: 2, a: 2}, {_id: 3, a: 3}]));
const filter1 = {
    a: {$gte: 2, $lte: 2}
};
const filter2 = {
    a: {$gte: 1, $lte: 2}
};
const sortPattern = {
    a: -1
};

// Create a cache entry using 'filter1'.
assert.eq(0, coll.getPlanCache().list().length, "Expected 0 cache entries");
const filter1Result = coll.find(filter1).sort(sortPattern).toArray();
const expectedFilter1Result = [{_id: 2, a: 2}];
assert.eq(expectedFilter1Result, filter1Result);
const cacheEntries = coll.getPlanCache().list();
assert.eq(1, cacheEntries.length, cacheEntries);
const cacheEntry = cacheEntries[0];

// Verify that our cache entry is pinned and active.
assert(cacheEntry.isPinned, cacheEntry);
assert(cacheEntry.isActive, cacheEntry);

// Capture the results for 'filter2' and verify that it used the same plan cache entry as 'filter1'.
const cacheResults = coll.find(filter2).sort(sortPattern).toArray();
const expectedFilter2Result = [{_id: 2, a: 2}, {_id: 1, a: 1}];
assert.eq(cacheResults, expectedFilter2Result);

// There should still be exactly one plan cache entry.
assert.eq(1, coll.getPlanCache().list().length, cacheEntries);

// The plan cache key and the query hashes of both queries should match.
const explain = coll.find(filter2).sort(sortPattern).explain();
const planCacheKey = cacheEntry.planCacheKey;
assert.neq(null, planCacheKey, cacheEntry);
assert.eq(planCacheKey, getPlanCacheKeyFromExplain(explain, db), explain);

const queryHash = cacheEntry.queryHash;
assert.neq(null, queryHash, cacheEntry);
assert.eq(queryHash, getQueryHashFromExplain(explain, db), explain);

// Clear the plan cache, and run 'filter2' again. This time, verify that we create a cache entry
// with the same planCacheKey and queryHash as before.
coll.getPlanCache().clear();
assert.eq(0, coll.getPlanCache().list().length, "Expected 0 cache entries");
const results = coll.find(filter2).sort(sortPattern).toArray();
const newCacheEntries = coll.getPlanCache().list();
assert.eq(1, newCacheEntries.length, "Expected 1 cache entry");
const newCacheEntry = newCacheEntries[0];
assert.eq(newCacheEntry.planCacheKey, planCacheKey, newCacheEntry);
assert.eq(newCacheEntry.queryHash, queryHash, newCacheEntry);

// The query should also return the same results as before.
assert.eq(results, cacheResults);

// Test that Infinity value in a filter should not be parameterized.
// These two queries have the same query shape but should not have a same plan cache key because
// the filter with a infinity value should not be eligible for auto-parameterization.
const filterWithInf = {
    "a": {$not: {$gt: NumberDecimal("Infinity")}}
};
const filterWithVeryLargeValue = {
    "a": {$not: {$gt: NumberDecimal("9.999999999999999999999999999999999E+6144")}}
};

assert.neq(
    getPlanCacheKeyFromExplain(coll.explain().aggregate([{$match: filterWithInf}]), db),
    getPlanCacheKeyFromExplain(coll.explain().aggregate([{$match: filterWithVeryLargeValue}]), db));

// Auto-parameterization should still apply if no infinity value is involved.
assert.eq(
    getPlanCacheKeyFromExplain(coll.explain().aggregate([{$match: {"a": {$not: {$gt: 1}}}}]), db),
    getPlanCacheKeyFromExplain(coll.explain().aggregate([{$match: filterWithVeryLargeValue}]), db));

const singleElemIn = {
    "a": {$in: [1]}
};
const multipleElemsIn = {
    "a": {$in: [1, 2]}
};
const multipleElemsIn2 = {
    "a": {$in: [3, 4]}
};
assert.neq(getPlanCacheKeyFromExplain(coll.explain().aggregate([{$match: singleElemIn}]), db),
           getPlanCacheKeyFromExplain(coll.explain().aggregate([{$match: multipleElemsIn}]), db));

// Auto-parameterization should still apply if there's no single-element $in query.
assert.eq(getPlanCacheKeyFromExplain(coll.explain().aggregate([{$match: multipleElemsIn}]), db),
          getPlanCacheKeyFromExplain(coll.explain().aggregate([{$match: multipleElemsIn2}]), db));

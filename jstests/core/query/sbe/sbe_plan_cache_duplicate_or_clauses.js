/**
 * This test was originally designed to reproduce SERVER-78752. It ensures that the correct plan is
 * added to the cache for similar queries that only differ in their IETs and if there is a
 * dependency between the values of the $or expression
 *
 * @tags: [
 *   # $planCacheStats requires readConcern:"local".
 *   assumes_read_concern_unchanged,
 *   # Plan cache state is node-local so the test assumes it is always reading from the same node.
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   # Plan cache state is node-local, so this test assumes it is always operating against the same
 *   # mongod.
 *   does_not_support_stepdowns,
 *   # $planCacheStats is not supported in transactions.
 *   does_not_support_transactions,
 *   # This test assumes that the queries are using the SBE plan cache.
 *   featureFlagSbeFull,
 *   assumes_balancer_off,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getPlanCacheShapeHashFromExplain, getPlanCacheShapeHashFromObject} from "jstests/libs/query/analyze_plan.js";

const coll = assertDropAndRecreateCollection(db, "sbe_plan_cache_duplicate_or_clauses");
const planCacheShapeHashSet = new Set();

const stash = (query) => {
    const planCacheShapeHash = getPlanCacheShapeHashFromExplain(query.explain());
    planCacheShapeHashSet.add(planCacheShapeHash);
    return query;
};

assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1}));

assert.commandWorked(coll.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(coll.insert({a: 1, b: 1, c: 2}));
assert.commandWorked(coll.insert({a: 1, b: 1, c: 3}));

// Show that this query returns 2 results when the plan cache is empty.
assert.eq(2, stash(coll.find({a: 1, b: 1, $or: [{c: 2}, {c: 3, d: {$eq: null}}]})).itcount());

// Create a cached plan. Run the query twice to make sure that the plan cache entry is active.
assert.eq(1, stash(coll.find({a: 1, b: 1, $or: [{c: 1}, {c: 1, d: {$eq: null}}]})).itcount());
assert.eq(1, coll.find({a: 1, b: 1, $or: [{c: 1}, {c: 1, d: {$eq: null}}]}).itcount());

// Check that each query has a separate entry in the plan cache
const cacheEntries = coll.getPlanCache().list().map(getPlanCacheShapeHashFromObject);
const matchingEntries = cacheEntries.filter((entry) => planCacheShapeHashSet.has(entry));
assert.eq(2, matchingEntries.length, [cacheEntries, planCacheShapeHashSet]);

// The query from above should still return 2 results.
assert.eq(2, coll.find({a: 1, b: 1, $or: [{c: 2}, {c: 3, d: {$eq: null}}]}).itcount());
